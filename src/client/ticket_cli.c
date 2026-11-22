#include "client.h"
#include "ticket_protocol.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_WORDS (3 + TICKET_BATCH_MAX * 2)

typedef struct {
    const char *command;
    const char *action;
    const char *usage;
} Command;

static const Command commands[] = {
    {"register", "REGISTER", "register USER [PASSWORD]"},
    {"login", "LOGIN", "login USER [PASSWORD]"},
    {"logout", "LOGOUT", "logout"},
    {"search", "SEARCH_MOVIES", "search \"TITLE\" [after_id=ID]"},
    {"browse", "BROWSE_MOVIES", "browse [genre=GENRE] [location=\"CITY\"] [date=YYYY-MM-DD] [time=HH:MM] [after_id=ID]"},
    {"showings", "GET_SHOWINGS", "showings MOVIE_ID [after_id=ID]"},
    {"seats", "GET_SEATS", "seats SHOWING_ID"},
    {"book", "BOOK_TICKETS", "book SHOWING_ID SEAT [SEAT ...]"},
    {"theaters", "LIST_THEATERS", "theaters [after_id=ID]"},
    {"add-movie", "ADD_MOVIE", "add-movie \"TITLE\" \"GENRE\" MINUTES \"DESCRIPTION\""},
    {"add-showing", "ADD_SHOWING", "add-showing MOVIE_ID THEATER_ID \"YYYY-MM-DD HH:MM\" [THEATER_ID \"YYYY-MM-DD HH:MM\" ...]"},
    {"update-showing", "UPDATE_SHOWING", "update-showing SHOWING_ID [theater_id=ID] [starts_at=\"YYYY-MM-DD HH:MM\"]"},
    {"create-user", "CREATE_USER", "create-user USER ROLE [PASSWORD]"},
    {"set-role", "SET_ROLE", "set-role USER_ID ROLE"},
};

static void show_help(void)
{
    puts("Guest and session");
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (i == 3)
            puts("Buyer");
        if (i == 8)
            puts("Sales manager");
        if (i == 12)
            puts("Admin");
        printf("  %s\n", commands[i].usage);
    }
    puts("  Roles: BUYER, MANAGER, ADMIN\n  help | exit");
}

static const Command *find_command(const char *word)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!strcasecmp(commands[i].command, word))
            return &commands[i];
    }
    return NULL;
}

static int split_words(char *line, char **words)
{
    char *read = line, *write = line;
    int count = 0;
    while (*read) {
        while (isspace((unsigned char)*read))
            read++;
        if (!*read)
            break;
        if (count == MAX_WORDS)
            return -1;

        words[count++] = write;
        int quoted = 0;
        while (*read && (quoted || !isspace((unsigned char)*read))) {
            char ch = *read++;
            if (ch == '"') {
                quoted = !quoted;
            } else {
                if (quoted && ch == '\\' && (*read == '"' || *read == '\\'))
                    ch = *read++;
                *write++ = ch;
            }
        }
        if (quoted)
            return -1;
        while (isspace((unsigned char)*read))
            read++;
        *write++ = '\0';
    }
    return count;
}

static void put_text(struct json_object *object, const char *key, const char *value)
{
    json_object_object_add(object, key, json_object_new_string(value));
}

static int put_id(struct json_object *object, const char *key, const char *value)
{
    if (!*value || value[strspn(value, "0123456789")])
        return 0;
    errno = 0;
    long number = strtol(value, NULL, 10);
    if (errno == ERANGE || number < 1 || number > INT_MAX)
        return 0;
    json_object_object_add(object, key, json_object_new_int((int)number));
    return 1;
}

static int put_options(struct json_object *data, char **words, int count,
                       const char *const *allowed, size_t allowed_count)
{
    for (int i = 0; i < count; i++) {
        char *equals = strchr(words[i], '=');
        if (!equals)
            return 0;
        *equals = '\0';
        const char *key = words[i];
        const char *value = equals + 1;
        int found = 0;
        for (size_t j = 0; j < allowed_count; j++) {
            if (!strcmp(key, allowed[j])) {
                found = 1;
                break;
            }
        }
        if (!found || ticket_field(data, key) || !*value)
            return 0;
        if (!strcmp(key, "after_id") || !strcmp(key, "theater_id")) {
            if (!put_id(data, key, value))
                return 0;
        } else {
            put_text(data, key, value);
        }
    }
    return 1;
}

static int put_credentials(struct json_object *data, char **words, int count,
                           int with_role, int interactive)
{
    int required = with_role ? 2 : 1;
    if (count != required + 1 && !(interactive && count == required))
        return 0;
    const char *password = count == required ? getpass("Password: ") : words[required];
    if (!password)
        return 0;
    put_text(data, "username", words[0]);
    put_text(data, "password", password);
    if (with_role)
        put_text(data, "role", words[1]);
    return 1;
}

static int put_slots(struct json_object *data, char **words, int count)
{
    if (count < 3 || count > 1 + 2 * TICKET_BATCH_MAX || count % 2 == 0 ||
        !put_id(data, "movie_id", words[0]))
        return 0;

    struct json_object *slots = json_object_new_array();
    json_object_object_add(data, "slots", slots);
    for (int i = 1; i < count; i += 2) {
        struct json_object *slot = json_object_new_object();
        json_object_array_add(slots, slot);
        if (!put_id(slot, "theater_id", words[i]))
            return 0;
        put_text(slot, "starts_at", words[i + 1]);
    }
    return 1;
}

static int put_seats(struct json_object *data, char **words, int count)
{
    if (count < 2 || count > 1 + TICKET_BATCH_MAX || !put_id(data, "showing_id", words[0]))
        return 0;

    struct json_object *seats = json_object_new_array();
    json_object_object_add(data, "seats", seats);
    for (int i = 1; i < count; i++) {
        if (words[i][0])
            words[i][0] = (char)toupper((unsigned char)words[i][0]);
        json_object_array_add(seats, json_object_new_string(words[i]));
    }
    return 1;
}

static int fill_data(const Command *command, char **words, int count,
                     struct json_object *data, int interactive)
{
    static const char *const page[] = {"after_id"};
    static const char *const filters[] = {"genre", "location", "date", "time", "after_id"};
    static const char *const changes[] = {"theater_id", "starts_at"};
    const char *name = command->command;
    if (!strcmp(name, "login") || !strcmp(name, "register") || !strcmp(name, "create-user"))
        return put_credentials(data, words, count, !strcmp(name, "create-user"), interactive);
    if (!strcmp(name, "logout"))
        return count == 0;
    if (!strcmp(name, "theaters"))
        return put_options(data, words, count, page, 1);
    if (!strcmp(name, "search")) {
        if (count < 1)
            return 0;
        put_text(data, "title", words[0]);
        return put_options(data, words + 1, count - 1, page, 1);
    }
    if (!strcmp(name, "browse"))
        return put_options(data, words, count, filters, sizeof(filters) / sizeof(filters[0]));
    if (!strcmp(name, "showings"))
        return count >= 1 && put_id(data, "movie_id", words[0]) &&
               put_options(data, words + 1, count - 1, page, 1);
    if (!strcmp(name, "seats"))
        return count == 1 && put_id(data, "showing_id", words[0]);
    if (!strcmp(name, "book"))
        return put_seats(data, words, count);
    if (!strcmp(name, "add-movie")) {
        if (count != 4 || !put_id(data, "duration", words[2]))
            return 0;
        put_text(data, "title", words[0]);
        put_text(data, "genre", words[1]);
        put_text(data, "description", words[3]);
        return 1;
    }
    if (!strcmp(name, "add-showing"))
        return put_slots(data, words, count);
    if (!strcmp(name, "update-showing"))
        return count >= 2 && put_id(data, "showing_id", words[0]) &&
               put_options(data, words + 1, count - 1, changes, sizeof(changes) / sizeof(changes[0]));
    if (!strcmp(name, "set-role")) {
        if (count != 2 || !put_id(data, "user_id", words[0]))
            return 0;
        put_text(data, "new_role", words[1]);
        return 1;
    }
    return 0;
}

static const char *text_field(struct json_object *object, const char *name)
{
    const char *text = ticket_text(ticket_field(object, name));
    return text ? text : "";
}

static int int_field(struct json_object *object, const char *name)
{
    return ticket_id(ticket_field(object, name));
}

static void show_seat_map(struct json_object *data)
{
    const char *layout = text_field(data, "layout");
    int rows, columns;
    if (sscanf(layout, "%dx%d", &rows, &columns) != 2 ||
        rows < 1 || rows > 26 || columns < 1 || columns > 20) {
        puts("Invalid seat map received.");
        return;
    }

    int taken[26][20] = {{0}};
    int available = rows * columns;
    struct json_object *booked = ticket_field(data, "booked");
    if (!json_object_is_type(booked, json_type_array))
        return;
    for (size_t i = 0; i < json_object_array_length(booked); i++) {
        const char *label = ticket_text(json_object_array_get_idx(booked, i));
        if (label && label[0] >= 'A' && label[0] < 'A' + rows) {
            int column = atoi(label + 1);
            if (column >= 1 && column <= columns && !taken[label[0] - 'A'][column - 1]) {
                taken[label[0] - 'A'][column - 1] = 1;
                available--;
            }
        }
    }

    struct json_object *showing = ticket_field(data, "showing");
    const char *starts_at = ticket_text(ticket_field(showing, "starts_at"));
    if (starts_at)
        printf("Theater #%d, %s\n", int_field(showing, "theater_id"), starts_at);

    puts("Screen");
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < columns; col++) {
            char label[4];
            snprintf(label, sizeof(label), "%c%d", 'A' + row, col + 1);
            printf(" %-3s", taken[row][col] ? "XX" : label);
        }
        putchar('\n');
    }
    puts(available ? "XX = booked. Choose available labels with the book command."
                   : "XX = booked. This showing is sold out.");
}

static void show_list(struct json_object *data, struct json_object *items, const char *key)
{
    size_t count = json_object_array_length(items);
    if (count == 0)
        printf("No %s found.\n", key);
    for (size_t i = 0; i < count; i++) {
        struct json_object *item = json_object_array_get_idx(items, i);
        int id = int_field(item, "id");
        if (!strcmp(key, "movies")) {
            printf("#%d %s | %s | %d min\n", id, text_field(item, "title"),
                   text_field(item, "genre"), int_field(item, "duration"));
        } else if (!strcmp(key, "showings")) {
            printf("#%d %s | %s | %s / %s | %s | %s\n", id, text_field(item, "title"),
                   text_field(item, "genre"), text_field(item, "cinema"), text_field(item, "theater"),
                   text_field(item, "location"), text_field(item, "starts_at"));
        } else {
            printf("#%d %s / %s | %s | %dx%d seats\n", id, text_field(item, "cinema"),
                   text_field(item, "theater"), text_field(item, "location"),
                   int_field(item, "rows"), int_field(item, "columns"));
        }
    }
    int next = int_field(data, "next_after_id");
    if (next)
        printf("More results available. Repeat the command with after_id=%d.\n", next);
}

static void show_success(const Command *command, struct json_object *data)
{
    const char *name = command->command;
    const char *const lists[] = {"movies", "showings", "theaters"};
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        struct json_object *items = ticket_field(data, lists[i]);
        if (json_object_is_type(items, json_type_array)) {
            show_list(data, items, lists[i]);
            return;
        }
    }

    if (!strcmp(name, "login")) {
        printf("Logged in as %s (user #%d).\n", text_field(data, "role"), int_field(data, "user_id"));
    } else if (!strcmp(name, "logout")) {
        puts("Logged out.");
    } else if (!strcmp(name, "register") || !strcmp(name, "create-user")) {
        printf("Created user #%d.\n", int_field(data, "user_id"));
    } else if (!strcmp(name, "set-role")) {
        printf("User #%d now has the %s role.\n",
               int_field(data, "user_id"), text_field(data, "role"));
    } else if (!strcmp(name, "add-movie")) {
        printf("Created movie #%d.\n", int_field(data, "movie_id"));
    } else if (!strcmp(name, "add-showing")) {
        struct json_object *ids = ticket_field(data, "showing_ids");
        printf("Created showings");
        if (json_object_is_type(ids, json_type_array)) {
            for (size_t i = 0; i < json_object_array_length(ids); i++)
                printf(" #%d", ticket_id(json_object_array_get_idx(ids, i)));
        }
        puts(".");
    } else if (!strcmp(name, "update-showing")) {
        struct json_object *showing = ticket_field(data, "showing");
        printf("Showing #%d updated to theater #%d at %s.\n", int_field(showing, "id"),
               int_field(showing, "theater_id"), text_field(showing, "starts_at"));
    } else if (!strcmp(name, "seats")) {
        show_seat_map(data);
    } else if (!strcmp(name, "book")) {
        printf("Booking #%d confirmed.\n", int_field(data, "booking_id"));
    }
}

static int exchange(TicketStream *stream, int id, const Command *command,
                    struct json_object *data, struct json_object *seat_maps)
{
    int showing_id = int_field(data, "showing_id");
    char showing_key[16];
    snprintf(showing_key, sizeof(showing_key), "%d", showing_id);
    if (!strcmp(command->command, "book")) {
        struct json_object *snapshot = ticket_field(seat_maps, showing_key);
        if (snapshot)
            json_object_object_add(data, "expected_showing", json_object_get(snapshot));
    }

    struct json_object *request = json_object_new_object();
    json_object_object_add(request, "id", json_object_new_int(id));
    put_text(request, "action", command->action);
    json_object_object_add(request, "data", data);

    size_t length;
    json_object_to_json_string_length(request, JSON_C_TO_STRING_PLAIN, &length);
    if (length >= TICKET_MESSAGE_MAX) {
        puts("Command is too large.");
        json_object_put(request);
        return 1;
    }
    int sent = ticket_send_json(stream->fd, request);
    json_object_put(request);
    if (sent != 0) {
        fputs("Could not send the command.\n", stderr);
        return 0;
    }

    char line[TICKET_MESSAGE_MAX];
    if (ticket_read_line(stream, line, &length) != TICKET_READ_OK) {
        fputs("Connection closed or invalid reply.\n", stderr);
        return 0;
    }

    struct json_object *response = ticket_parse_json(line, length);
    const char *status = text_field(response, "status");
    int ok = !strcmp(status, "OK");
    struct json_object *reply = ticket_field(response, ok ? "data" : "error");
    const char *code = ticket_text(ticket_field(reply, "code"));
    const char *message = ticket_text(ticket_field(reply, "message"));
    if (int_field(response, "id") != id || !json_object_is_type(reply, json_type_object) ||
        (!ok && (strcmp(status, "ERROR") || !code || !message))) {
        json_object_put(response);
        fputs("Invalid reply from the server.\n", stderr);
        return 0;
    }

    if (ok) {
        if (!strcmp(command->command, "seats")) {
            struct json_object *snapshot = ticket_field(reply, "showing");
            if (json_object_is_type(snapshot, json_type_object))
                json_object_object_add(seat_maps, showing_key, json_object_get(snapshot));
        }
        show_success(command, reply);
    } else {
        printf("%s: %s\n", code, message);
    }
    json_object_put(response);
    return 1;
}

int client_main_loop(int sockfd)
{
    TicketStream stream = {.fd = sockfd};
    int interactive = isatty(STDIN_FILENO);
    int id = 1;
    int status = 0;
    struct json_object *seat_maps = json_object_new_object();
    char *line = NULL;
    size_t capacity = 0;
    if (interactive)
        puts("Movie tickets. Type help to see commands.");

    for (;;) {
        if (interactive) {
            fputs("tickets> ", stdout);
            fflush(stdout);
        }
        if (getline(&line, &capacity, stdin) < 0) {
            if (ferror(stdin)) {
                perror("Could not read command");
                status = -1;
            }
            break;
        }

        char *words[MAX_WORDS];
        int count = split_words(line, words);
        if (count < 0) {
            puts("Invalid quoting or too many arguments. Type help for usage.");
            continue;
        }
        if (count == 0)
            continue;
        if (!strcasecmp(words[0], "exit") || !strcasecmp(words[0], "quit"))
            break;
        if (!strcasecmp(words[0], "help")) {
            show_help();
            continue;
        }

        const Command *command = find_command(words[0]);
        if (!command) {
            puts("Unknown command. Type help for usage.");
            continue;
        }
        struct json_object *data = json_object_new_object();
        if (!fill_data(command, words + 1, count - 1, data, interactive)) {
            printf("Usage: %s\n", command->usage);
            json_object_put(data);
            continue;
        }
        if (!exchange(&stream, id, command, data, seat_maps)) {
            status = -1;
            break;
        }
        id = id == INT_MAX ? 1 : id + 1;
    }
    free(line);
    json_object_put(seat_maps);
    return status;
}
