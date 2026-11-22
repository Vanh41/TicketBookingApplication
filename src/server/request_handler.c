#include "ticket_server.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef enum {
    GUEST = 1 << 0,
    BUYER = 1 << 1,
    MANAGER = 1 << 2,
    ADMIN = 1 << 3
} Role;

typedef TicketResult (*TicketHandler)(TicketSession *, struct json_object *);

typedef struct {
    const char *name;
    unsigned roles;
    TicketHandler handle;
} Action;

static const Action actions[] = {
    {"REGISTER",       GUEST,                   ticket_register},
    {"LOGIN",          GUEST,                   ticket_login},
    {"LOGOUT",         BUYER | MANAGER | ADMIN, ticket_logout},
    {"SEARCH_MOVIES",  BUYER | MANAGER,         ticket_search},
    {"BROWSE_MOVIES",  BUYER | MANAGER,         ticket_browse},
    {"GET_SHOWINGS",   BUYER | MANAGER,         ticket_showings},
    {"GET_SEATS",      BUYER,                   ticket_get_seats},
    {"BOOK_TICKETS",   BUYER,                   ticket_book},
    {"LIST_THEATERS",  MANAGER,                 ticket_theaters},
    {"ADD_MOVIE",      MANAGER,                 ticket_add_movie},
    {"ADD_SHOWING",    MANAGER,                 ticket_add_showing},
    {"UPDATE_SHOWING", MANAGER,                 ticket_update_showing},
    {"CREATE_USER",    ADMIN,                   ticket_create_user},
    {"SET_ROLE",       ADMIN,                   ticket_set_role},
};

static const Action *find_action(const char *name)
{
    if (name) {
        for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
            if (!strcmp(name, actions[i].name))
                return &actions[i];
        }
    }
    return NULL;
}

static Role role_from_text(const char *role)
{
    if (!strcmp(role, "BUYER"))
        return BUYER;
    if (!strcmp(role, "MANAGER"))
        return MANAGER;
    if (!strcmp(role, "ADMIN"))
        return ADMIN;
    return 0;
}

static TicketResult authorize(TicketSession *session, const Action *action)
{
    if (!session->user_id) {
        if (action->roles & GUEST)
            return (TicketResult){0};
        return ticket_error("NOT_LOGGED_IN", "Log in to use this action.");
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(session->db, "SELECT role FROM users WHERE id=?", -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(session->db);

    sqlite3_bind_int(stmt, 1, session->user_id);
    int rc = sqlite3_step(stmt);
    TicketResult result = {0};
    if (rc == SQLITE_ROW) {
        Role role = role_from_text((const char *)sqlite3_column_text(stmt, 0));
        if (!(action->roles & role))
            result = ticket_error("PERMISSION_DENIED", "Your role cannot use this action.");
    } else if (rc == SQLITE_DONE) {
        session->user_id = 0;
        result = ticket_error("NOT_LOGGED_IN", "Log in to use this action.");
    } else {
        result = ticket_db_error(session->db);
    }
    sqlite3_finalize(stmt);
    return result;
}

static int send_result(int fd, int id, TicketResult result)
{
    struct json_object *response = json_object_new_object();
    json_object_object_add(response, "id", id ? json_object_new_int(id) : NULL);
    json_object_object_add(response, "status", json_object_new_string(result.code ? "ERROR" : "OK"));
    if (result.code) {
        struct json_object *error = json_object_new_object();
        json_object_object_add(error, "code", json_object_new_string(result.code));
        json_object_object_add(error, "message", json_object_new_string(result.message));
        json_object_object_add(response, "error", error);
    } else {
        json_object_object_add(response, "data", result.data);
    }

    int sent = ticket_send_json(fd, response);
    char message[TICKET_MESSAGE_MAX + 64];
    snprintf(message, sizeof(message), "%s fd=%d %s", sent == 0 ? "SEND" : "SEND_FAILED",
             fd, json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN));
    log_activity(message);
    json_object_put(response);
    return sent;
}

static struct json_object *redact_passwords(struct json_object *value)
{
    if (json_object_is_type(value, json_type_object)) {
        struct json_object *safe = json_object_new_object();
        json_object_object_foreach(value, key, child) {
            struct json_object *entry = !strcasecmp(key, "password")
                ? json_object_new_string("<redacted>") : redact_passwords(child);
            json_object_object_add(safe, key, entry);
        }
        return safe;
    }

    if (json_object_is_type(value, json_type_array)) {
        struct json_object *safe = json_object_new_array();
        for (size_t i = 0; i < json_object_array_length(value); i++)
            json_object_array_add(safe, redact_passwords(json_object_array_get_idx(value, i)));
        return safe;
    }

    return json_object_get(value);
}

static void log_request(int fd, int id, const Action *action, struct json_object *data)
{
    char message[TICKET_MESSAGE_MAX + 64];
    if (action && json_object_is_type(data, json_type_object)) {
        struct json_object *safe = redact_passwords(data);
        snprintf(message, sizeof(message), "RECV fd=%d id=%d action=%s data=%s",
                 fd, id, action->name, json_object_to_json_string_ext(safe, JSON_C_TO_STRING_PLAIN));
        json_object_put(safe);
    } else {
        snprintf(message, sizeof(message), "RECV fd=%d id=%d action=%s data=<omitted>",
                 fd, id, action ? action->name : "<invalid or unknown>");
    }
    log_activity(message);
}

int ticket_handle_request(TicketSession *session, int fd, const char *line, size_t length)
{
    struct json_object *request = ticket_parse_json(line, length);
    int id = ticket_id(ticket_field(request, "id"));
    const char *name = ticket_text(ticket_field(request, "action"));
    struct json_object *data = ticket_field(request, "data");
    const Action *action = find_action(name);
    log_request(fd, id, action, data);

    TicketResult result;
    if (!id || !name || !json_object_is_type(data, json_type_object)) {
        result = ticket_invalid("Expected a JSON object with a positive id, action, and data object.");
    } else if (!action) {
        result = ticket_error("UNKNOWN_ACTION", "Unknown action.");
    } else {
        result = authorize(session, action);
        if (!result.code)
            result = action->handle(session, data);
    }
    json_object_put(request);
    return send_result(fd, id, result);
}

int ticket_reject_large_request(int fd)
{
    char message[96];
    snprintf(message, sizeof(message), "RECV fd=%d message exceeds %d bytes", fd, TICKET_MESSAGE_MAX);
    log_activity(message);
    snprintf(message, sizeof(message), "The message exceeds %d bytes.", TICKET_MESSAGE_MAX);
    return send_result(fd, 0, ticket_error("MESSAGE_TOO_LARGE", message));
}
