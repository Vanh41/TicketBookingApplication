#include "ticket_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define TICKET_DB_FILE "data/tickets.sqlite3"

static const char schema[] =
    "BEGIN IMMEDIATE;"
    "CREATE TABLE IF NOT EXISTS users ("
    " id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,"
    " password_hash BLOB NOT NULL, salt BLOB NOT NULL,"
    " role TEXT NOT NULL CHECK(role IN ('BUYER','MANAGER','ADMIN'))"
    ");"
    "CREATE TABLE IF NOT EXISTS movies ("
    " id INTEGER PRIMARY KEY, title TEXT NOT NULL, genre TEXT NOT NULL,"
    " description TEXT NOT NULL, duration INTEGER NOT NULL CHECK(duration BETWEEN 1 AND 500)"
    ");"
    "CREATE TABLE IF NOT EXISTS cinemas ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL, location TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS theaters ("
    " id INTEGER PRIMARY KEY, cinema_id INTEGER NOT NULL REFERENCES cinemas(id),"
    " name TEXT NOT NULL, rows INTEGER NOT NULL CHECK(rows BETWEEN 1 AND 26),"
    " columns INTEGER NOT NULL CHECK(columns BETWEEN 1 AND 20)"
    ");"
    "CREATE TABLE IF NOT EXISTS showings ("
    " id INTEGER PRIMARY KEY, movie_id INTEGER NOT NULL REFERENCES movies(id),"
    " theater_id INTEGER NOT NULL REFERENCES theaters(id), starts_at TEXT NOT NULL,"
    " UNIQUE(theater_id, starts_at)"
    ");"
    "CREATE INDEX IF NOT EXISTS showings_by_movie ON showings(movie_id, id);"
    "CREATE TABLE IF NOT EXISTS bookings ("
    " id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
    " showing_id INTEGER NOT NULL REFERENCES showings(id),"
    " created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, UNIQUE(id, showing_id)"
    ");"
    "CREATE INDEX IF NOT EXISTS bookings_by_showing ON bookings(showing_id);"
    "CREATE TABLE IF NOT EXISTS booked_seats ("
    " booking_id INTEGER NOT NULL, showing_id INTEGER NOT NULL, seat TEXT NOT NULL,"
    " PRIMARY KEY(showing_id, seat),"
    " FOREIGN KEY(booking_id, showing_id) REFERENCES bookings(id, showing_id)"
    ");"
    "INSERT OR IGNORE INTO cinemas VALUES (1, 'Cinema Hanoi', 'Hanoi'),"
    " (2, 'Cinema Da Nang', 'Da Nang');"
    "INSERT OR IGNORE INTO theaters VALUES (1, 1, 'Room 1', 6, 10), (2, 1, 'Room 2', 5, 8),"
    " (3, 2, 'Room 1', 6, 10), (4, 2, 'Room 2', 4, 8);"
    "COMMIT;";

static void case_match(sqlite3_context *context, int count, sqlite3_value **values)
{
    (void)count;
    const char *text = (const char *)sqlite3_value_text(values[0]);
    const char *query = (const char *)sqlite3_value_text(values[1]);
    if (!text || !query) {
        sqlite3_result_int(context, 0);
        return;
    }

    wchar_t folded_text[501], folded_query[501];
    size_t text_length = mbstowcs(folded_text, text, 501);
    size_t query_length = mbstowcs(folded_query, query, 501);
    if (text_length >= 501 || query_length >= 501) {
        sqlite3_result_int(context, 0);
        return;
    }

    for (size_t i = 0; i < text_length; i++)
        folded_text[i] = towlower(folded_text[i]);
    for (size_t i = 0; i < query_length; i++)
        folded_query[i] = towlower(folded_query[i]);

    int match = sqlite3_user_data(context)
        ? wcscmp(folded_text, folded_query) == 0
        : wcsstr(folded_text, folded_query) != NULL;
    sqlite3_result_int(context, match);
}

int ticket_open_db(sqlite3 **db)
{
    static const int exact = 1;
    if (sqlite3_open(TICKET_DB_FILE, db) == SQLITE_OK &&
        sqlite3_busy_timeout(*db, 5000) == SQLITE_OK &&
        sqlite3_exec(*db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) == SQLITE_OK &&
        sqlite3_create_function(*db, "text_contains", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                NULL, case_match, NULL, NULL) == SQLITE_OK &&
        sqlite3_create_function(*db, "text_equals", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                (void *)&exact, case_match, NULL, NULL) == SQLITE_OK)
        return 0;

    ticket_db_error(*db);
    sqlite3_close(*db);
    *db = NULL;
    return -1;
}

int ticket_init_db(sqlite3 *db)
{
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL) == SQLITE_OK &&
        sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK)
        return 0;

    ticket_db_error(db);
    return -1;
}

TicketResult ticket_ok(struct json_object *data)
{
    return (TicketResult){.data = data ? data : json_object_new_object()};
}

TicketResult ticket_error(const char *code, const char *message)
{
    TicketResult result = {.code = code};
    snprintf(result.message, sizeof(result.message), "%s", message);
    return result;
}

TicketResult ticket_invalid(const char *message)
{
    return ticket_error("INVALID_REQUEST", message);
}

TicketResult ticket_db_error(sqlite3 *db)
{
    fprintf(stderr, "Database error: %s\n", db ? sqlite3_errmsg(db) : "could not open database");
    return ticket_error("INTERNAL_ERROR", "The server could not complete this request.");
}

TicketResult ticket_created(const char *key, int id)
{
    struct json_object *data = json_object_new_object();
    json_object_object_add(data, key, json_object_new_int(id));
    return ticket_ok(data);
}

TicketResult ticket_commit(sqlite3 *db, TicketResult result)
{
    if (!result.code && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
        return result;

    if (!result.code) {
        json_object_put(result.data);
        result = ticket_db_error(db);
    }
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return result;
}

int ticket_valid_text(const char *text, size_t min, size_t max)
{
    if (!text)
        return 0;
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 32 || *p == 127)
            return 0;
        if ((*p & 0xc0) != 0x80)
            count++;
    }
    return count >= min && count <= max;
}

static int digits(const char *s, int length)
{
    int value = 0;
    for (int i = 0; i < length; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        value = value * 10 + s[i] - '0';
    }
    return value;
}

int ticket_valid_date(const char *date)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-')
        return 0;

    int year = digits(date, 4);
    int month = digits(date + 5, 2);
    int day = digits(date + 8, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1)
        return 0;

    int leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap);
}

int ticket_valid_time(const char *time)
{
    if (!time || strlen(time) != 5 || time[2] != ':')
        return 0;
    int hour = digits(time, 2);
    int minute = digits(time + 3, 2);
    return hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
}

int ticket_valid_start(const char *start)
{
    if (!start || strlen(start) != 16 || start[10] != ' ')
        return 0;
    char date[11];
    memcpy(date, start, 10);
    date[10] = '\0';
    return ticket_valid_date(date) && ticket_valid_time(start + 11);
}
