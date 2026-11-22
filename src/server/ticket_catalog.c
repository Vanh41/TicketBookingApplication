#include "ticket_server.h"

#include <stdio.h>
#include <string.h>

#define SHOWING_COLUMNS "s.id, s.movie_id, m.title, m.genre, c.name AS cinema, t.name AS theater, c.location, s.starts_at "
#define SHOWING_TABLES "FROM showings s JOIN movies m ON m.id=s.movie_id " \
                       "JOIN theaters t ON t.id=s.theater_id JOIN cinemas c ON c.id=t.cinema_id "
#define PAGE_MAX_ROWS 30
#define PAGE_MAX_BYTES 3600

static void bind_text(sqlite3_stmt *stmt, int index, const char *text)
{
    sqlite3_bind_text(stmt, index, text, -1, SQLITE_STATIC);
}

static struct json_object *row_object(sqlite3_stmt *stmt)
{
    struct json_object *row = json_object_new_object();
    for (int i = 0; i < sqlite3_column_count(stmt); i++) {
        struct json_object *value = sqlite3_column_type(stmt, i) == SQLITE_INTEGER
            ? json_object_new_int64(sqlite3_column_int64(stmt, i))
            : json_object_new_string((const char *)sqlite3_column_text(stmt, i));
        json_object_object_add(row, sqlite3_column_name(stmt, i), value);
    }
    return row;
}

static TicketResult query_page(sqlite3 *db, sqlite3_stmt *stmt, const char *key)
{
    struct json_object *items = json_object_new_array();
    size_t bytes = 0;
    int last_id = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct json_object *row = row_object(stmt);
        size_t length;
        json_object_to_json_string_length(row, JSON_C_TO_STRING_PLAIN, &length);
        if (json_object_array_length(items) >= PAGE_MAX_ROWS || bytes + length + 1 > PAGE_MAX_BYTES) {
            json_object_put(row);
            break;
        }
        json_object_array_add(items, row);
        bytes += length + 1;
        last_id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        json_object_put(items);
        return ticket_db_error(db);
    }

    struct json_object *data = json_object_new_object();
    json_object_object_add(data, key, items);
    if (rc == SQLITE_ROW)
        json_object_object_add(data, "next_after_id", json_object_new_int(last_id));
    return ticket_ok(data);
}

static int page_cursor(struct json_object *data)
{
    struct json_object *value;
    if (!json_object_object_get_ex(data, "after_id", &value))
        return 0;
    int id = ticket_id(value);
    return id ? id : -1;
}

static int exists_id(sqlite3 *db, const char *sql, int id)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

TicketResult ticket_search(TicketSession *session, struct json_object *data)
{
    int after = page_cursor(data);
    const char *title = ticket_text(ticket_field(data, "title"));
    if (after < 0 || !ticket_valid_text(title, 1, 100))
        return ticket_invalid("Expected a title of 1–100 characters and an optional positive after_id.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(session->db,
            "SELECT id,title,genre,duration FROM movies WHERE id>? AND text_contains(title,?) ORDER BY id",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(session->db);

    sqlite3_bind_int(stmt, 1, after);
    bind_text(stmt, 2, title);
    return query_page(session->db, stmt, "movies");
}

static int optional_filter(struct json_object *data, const char *key, const char **out)
{
    struct json_object *value;
    *out = NULL;
    if (!json_object_object_get_ex(data, key, &value))
        return 1;
    *out = ticket_text(value);
    return *out != NULL;
}

static int known_filter(sqlite3 *db, const char *sql, const char *filter)
{
    if (!filter)
        return 1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    bind_text(stmt, 1, filter);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

TicketResult ticket_browse(TicketSession *session, struct json_object *data)
{
    const char *genre, *location, *date, *time;
    int after = page_cursor(data);
    if (after < 0)
        return ticket_invalid("Expected a positive after_id.");
    if (!optional_filter(data, "genre", &genre) || (genre && !ticket_valid_text(genre, 1, 30)) ||
        !optional_filter(data, "location", &location) || (location && !ticket_valid_text(location, 1, 30)) ||
        !optional_filter(data, "date", &date) || (date && !ticket_valid_date(date)) ||
        !optional_filter(data, "time", &time) || (time && !ticket_valid_time(time)))
        return ticket_error("INVALID_FILTER", "Expected genre, location, date (YYYY-MM-DD), or time (HH:MM).");

    sqlite3 *db = session->db;
    int genre_known = known_filter(db, "SELECT 1 FROM movies WHERE text_equals(genre,?) LIMIT 1", genre);
    int location_known = known_filter(db, "SELECT 1 FROM cinemas WHERE text_equals(location,?) LIMIT 1", location);
    if (genre_known < 0 || location_known < 0)
        return ticket_db_error(db);
    if (!genre_known || !location_known)
        return ticket_error("INVALID_FILTER", "Unknown genre or cinema location.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT " SHOWING_COLUMNS SHOWING_TABLES
            "WHERE s.id>?1 AND (?2 IS NULL OR text_equals(m.genre,?2)) "
            "AND (?3 IS NULL OR text_equals(c.location,?3)) "
            "AND (?4 IS NULL OR substr(s.starts_at,1,10)=?4) "
            "AND (?5 IS NULL OR substr(s.starts_at,12,5)=?5) ORDER BY s.id",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_int(stmt, 1, after);
    bind_text(stmt, 2, genre);
    bind_text(stmt, 3, location);
    bind_text(stmt, 4, date);
    bind_text(stmt, 5, time);
    return query_page(db, stmt, "showings");
}

TicketResult ticket_showings(TicketSession *session, struct json_object *data)
{
    int movie_id = ticket_id(ticket_field(data, "movie_id"));
    int after = page_cursor(data);
    if (!movie_id || after < 0)
        return ticket_invalid("Expected a positive movie_id and an optional positive after_id.");

    sqlite3 *db = session->db;
    int found = exists_id(db, "SELECT 1 FROM movies WHERE id=?", movie_id);
    if (found < 0)
        return ticket_db_error(db);
    if (!found)
        return ticket_error("NOT_FOUND", "Movie not found.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT " SHOWING_COLUMNS SHOWING_TABLES
            "WHERE s.movie_id=? AND s.id>? ORDER BY s.id",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_int(stmt, 1, movie_id);
    sqlite3_bind_int(stmt, 2, after);
    return query_page(db, stmt, "showings");
}

TicketResult ticket_theaters(TicketSession *session, struct json_object *data)
{
    int after = page_cursor(data);
    if (after < 0)
        return ticket_invalid("Expected a positive after_id.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(session->db,
            "SELECT t.id, c.name AS cinema, c.location, t.name AS theater, t.rows, t.columns "
            "FROM theaters t JOIN cinemas c ON c.id=t.cinema_id WHERE t.id>? ORDER BY t.id",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(session->db);
    sqlite3_bind_int(stmt, 1, after);
    return query_page(session->db, stmt, "theaters");
}

TicketResult ticket_add_movie(TicketSession *session, struct json_object *data)
{
    const char *title = ticket_text(ticket_field(data, "title"));
    const char *genre = ticket_text(ticket_field(data, "genre"));
    const char *description = ticket_text(ticket_field(data, "description"));
    int duration = ticket_id(ticket_field(data, "duration"));
    if (!ticket_valid_text(title, 1, 100) || !ticket_valid_text(genre, 1, 30) ||
        !ticket_valid_text(description, 0, 500) || duration < 1 || duration > 500)
        return ticket_invalid("Expected title, genre, description, and duration (1–500 minutes).");

    sqlite3 *db = session->db;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "INSERT INTO movies(title,genre,description,duration) VALUES(?,?,?,?)",
                           -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    bind_text(stmt, 1, title);
    bind_text(stmt, 2, genre);
    bind_text(stmt, 3, description);
    sqlite3_bind_int(stmt, 4, duration);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return ticket_db_error(db);
    return ticket_created("movie_id", (int)sqlite3_last_insert_rowid(db));
}

static int duration_for_movie(sqlite3 *db, int movie_id)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT duration FROM movies WHERE id=?", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(stmt, 1, movie_id);
    int rc = sqlite3_step(stmt);
    int duration = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : rc == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return duration;
}

static TicketResult check_slot(sqlite3 *db, int showing_id, int theater_id, const char *start, int duration)
{
    int found = exists_id(db, "SELECT 1 FROM theaters WHERE id=?", theater_id);
    if (found < 0)
        return ticket_db_error(db);
    if (!found)
        return ticket_error("NOT_FOUND", "Theater not found.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM showings s JOIN movies m ON m.id=s.movie_id "
            "WHERE s.theater_id=?1 AND s.id<>?2 "
            "AND CAST(strftime('%s',s.starts_at) AS INTEGER) < CAST(strftime('%s',?3) AS INTEGER)+?4*60 "
            "AND CAST(strftime('%s',?3) AS INTEGER) < CAST(strftime('%s',s.starts_at) AS INTEGER)+m.duration*60 "
            "LIMIT 1", -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_int(stmt, 1, theater_id);
    sqlite3_bind_int(stmt, 2, showing_id);
    bind_text(stmt, 3, start);
    sqlite3_bind_int(stmt, 4, duration);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW)
        return ticket_error("SHOWING_CONFLICT", "The theater already has an overlapping showing.");
    if (rc != SQLITE_DONE)
        return ticket_db_error(db);
    return (TicketResult){0};
}

typedef struct {
    int theater_id;
    const char *starts_at;
} ShowingSlot;

TicketResult ticket_add_showing(TicketSession *session, struct json_object *data)
{
    int movie_id = ticket_id(ticket_field(data, "movie_id"));
    struct json_object *slots = ticket_field(data, "slots");
    if (!movie_id || !json_object_is_type(slots, json_type_array))
        return ticket_invalid("Expected movie_id and an array of showing slots.");

    size_t count = json_object_array_length(slots);
    if (count < 1 || count > TICKET_BATCH_MAX)
        return ticket_invalid("Supply 1–10 showing slots.");

    ShowingSlot requests[TICKET_BATCH_MAX];
    for (size_t i = 0; i < count; i++) {
        struct json_object *slot = json_object_array_get_idx(slots, i);
        int theater_id = ticket_id(ticket_field(slot, "theater_id"));
        const char *start = ticket_text(ticket_field(slot, "starts_at"));
        if (!theater_id || !ticket_valid_start(start))
            return ticket_invalid("Each slot needs theater_id and starts_at (YYYY-MM-DD HH:MM).");
        requests[i] = (ShowingSlot){theater_id, start};
    }

    sqlite3 *db = session->db;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    int duration = duration_for_movie(db, movie_id);
    if (duration <= 0)
        return ticket_commit(db, duration < 0 ? ticket_db_error(db)
                                            : ticket_error("NOT_FOUND", "Movie not found."));

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "INSERT INTO showings(movie_id,theater_id,starts_at) VALUES(?,?,?)",
                           -1, &stmt, NULL) != SQLITE_OK)
        return ticket_commit(db, ticket_db_error(db));

    struct json_object *ids = json_object_new_array();
    TicketResult result = {0};
    for (size_t i = 0; i < count; i++) {
        ShowingSlot slot = requests[i];
        result = check_slot(db, 0, slot.theater_id, slot.starts_at, duration);
        if (result.code)
            break;

        sqlite3_bind_int(stmt, 1, movie_id);
        sqlite3_bind_int(stmt, 2, slot.theater_id);
        bind_text(stmt, 3, slot.starts_at);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            result = ticket_db_error(db);
            break;
        }
        json_object_array_add(ids, json_object_new_int64(sqlite3_last_insert_rowid(db)));
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    if (result.code) {
        json_object_put(ids);
    } else {
        result = ticket_ok(NULL);
        json_object_object_add(result.data, "showing_ids", ids);
    }
    return ticket_commit(db, result);
}

static TicketResult update_showing(sqlite3 *db, int id, int theater_id, const char *start)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT s.theater_id,s.starts_at,m.duration,EXISTS(SELECT 1 FROM bookings b WHERE b.showing_id=s.id) "
            "FROM showings s JOIN movies m ON m.id=s.movie_id WHERE s.id=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE ? ticket_error("NOT_FOUND", "Showing not found.") : ticket_db_error(db);
    }

    if (!theater_id)
        theater_id = sqlite3_column_int(stmt, 0);
    char starts_at[17];
    snprintf(starts_at, sizeof(starts_at), "%s",
             start ? start : (const char *)sqlite3_column_text(stmt, 1));
    int duration = sqlite3_column_int(stmt, 2);
    int booked = sqlite3_column_int(stmt, 3);
    sqlite3_finalize(stmt);
    if (booked)
        return ticket_error("SHOWING_HAS_BOOKINGS", "A showing with bookings cannot be changed.");

    TicketResult conflict = check_slot(db, id, theater_id, starts_at, duration);
    if (conflict.code)
        return conflict;

    if (sqlite3_prepare_v2(db, "UPDATE showings SET theater_id=?,starts_at=? WHERE id=?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);
    sqlite3_bind_int(stmt, 1, theater_id);
    bind_text(stmt, 2, starts_at);
    sqlite3_bind_int(stmt, 3, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return ticket_db_error(db);

    struct json_object *showing = json_object_new_object();
    json_object_object_add(showing, "id", json_object_new_int(id));
    json_object_object_add(showing, "theater_id", json_object_new_int(theater_id));
    json_object_object_add(showing, "starts_at", json_object_new_string(starts_at));
    TicketResult result = ticket_ok(NULL);
    json_object_object_add(result.data, "showing", showing);
    return result;
}

TicketResult ticket_update_showing(TicketSession *session, struct json_object *data)
{
    int id = ticket_id(ticket_field(data, "showing_id"));
    struct json_object *theater_value, *start_value;
    int has_theater = json_object_object_get_ex(data, "theater_id", &theater_value);
    int has_start = json_object_object_get_ex(data, "starts_at", &start_value);
    int theater_id = has_theater ? ticket_id(theater_value) : 0;
    const char *start = has_start ? ticket_text(start_value) : NULL;
    if (!id || (!has_theater && !has_start) || (has_theater && !theater_id) ||
        (has_start && !ticket_valid_start(start)))
        return ticket_invalid("Expected showing_id and a new theater_id and/or starts_at.");

    sqlite3 *db = session->db;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return ticket_db_error(db);
    return ticket_commit(db, update_showing(db, id, theater_id, start));
}
