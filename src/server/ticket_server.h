#ifndef TICKET_SERVER_H
#define TICKET_SERVER_H

#include "ticket_protocol.h"

#include <sqlite3.h>

typedef struct {
    sqlite3 *db;
    int user_id;
} TicketSession;

typedef struct {
    struct json_object *data;
    const char *code;
    char message[160];
} TicketResult;

int ticket_handle_request(TicketSession *session, int fd, const char *line, size_t length);
int ticket_reject_large_request(int fd);

int ticket_open_db(sqlite3 **db);
int ticket_init_db(sqlite3 *db);
int ticket_bootstrap_admin(sqlite3 *db, const char *username, const char *password);

TicketResult ticket_ok(struct json_object *data);
TicketResult ticket_error(const char *code, const char *message);
TicketResult ticket_invalid(const char *message);
TicketResult ticket_db_error(sqlite3 *db);
TicketResult ticket_created(const char *key, int id);
TicketResult ticket_commit(sqlite3 *db, TicketResult result);
int ticket_valid_text(const char *text, size_t min, size_t max);
int ticket_valid_date(const char *date);
int ticket_valid_time(const char *time);
int ticket_valid_start(const char *start);

TicketResult ticket_register(TicketSession *, struct json_object *);
TicketResult ticket_login(TicketSession *, struct json_object *);
TicketResult ticket_logout(TicketSession *, struct json_object *);
TicketResult ticket_create_user(TicketSession *, struct json_object *);
TicketResult ticket_set_role(TicketSession *, struct json_object *);

TicketResult ticket_search(TicketSession *, struct json_object *);
TicketResult ticket_browse(TicketSession *, struct json_object *);
TicketResult ticket_showings(TicketSession *, struct json_object *);
TicketResult ticket_theaters(TicketSession *, struct json_object *);
TicketResult ticket_add_movie(TicketSession *, struct json_object *);
TicketResult ticket_add_showing(TicketSession *, struct json_object *);
TicketResult ticket_update_showing(TicketSession *, struct json_object *);

TicketResult ticket_get_seats(TicketSession *, struct json_object *);
TicketResult ticket_book(TicketSession *, struct json_object *);

void log_activity(const char *message);

#endif
