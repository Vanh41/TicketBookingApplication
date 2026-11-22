#include "ticket_server.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int theater_id;
    const char *starts_at;
} ShowingSnapshot;

TicketResult ticket_get_seats(TicketSession *session, struct json_object *data)
{
    int showing_id = ticket_id(ticket_field(data, "showing_id"));
    if (!showing_id)
        return ticket_invalid("Expected a positive showing_id.");

    sqlite3 *db = session->db;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT t.rows,t.columns,b.seat,s.theater_id,s.starts_at "
            "FROM showings s JOIN theaters t ON t.id=s.theater_id "
            "LEFT JOIN booked_seats b ON b.showing_id=s.id WHERE s.id=? ORDER BY b.seat",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_int(stmt, 1, showing_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE ? ticket_error("NOT_FOUND", "Showing not found.") : ticket_db_error(db);
    }

    char layout[16];
    snprintf(layout, sizeof(layout), "%dx%d", sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1));
    struct json_object *showing = json_object_new_object();
    json_object_object_add(showing, "theater_id", json_object_new_int(sqlite3_column_int(stmt, 3)));
    json_object_object_add(showing, "starts_at",
                           json_object_new_string((const char *)sqlite3_column_text(stmt, 4)));
    struct json_object *booked = json_object_new_array();
    do {
        const unsigned char *seat = sqlite3_column_text(stmt, 2);
        if (seat)
            json_object_array_add(booked, json_object_new_string((const char *)seat));
    } while ((rc = sqlite3_step(stmt)) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        json_object_put(booked);
        json_object_put(showing);
        return ticket_db_error(db);
    }

    TicketResult result = ticket_ok(NULL);
    json_object_object_add(result.data, "layout", json_object_new_string(layout));
    json_object_object_add(result.data, "booked", booked);
    json_object_object_add(result.data, "showing", showing);
    return result;
}

static int seat_column(const char *seat)
{
    if (!seat || seat[0] < 'A' || seat[0] > 'Z' || seat[1] < '1' || seat[1] > '9')
        return 0;
    int column = seat[1] - '0';
    if (seat[2] == '\0')
        return column;
    if (seat[2] < '0' || seat[2] > '9' || seat[3] != '\0')
        return 0;
    return column * 10 + seat[2] - '0';
}

static TicketResult create_booking(TicketSession *session, int showing_id,
                                   const char **seats, size_t count, ShowingSnapshot expected)
{
    sqlite3 *db = session->db;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT t.rows,t.columns,s.theater_id,s.starts_at "
            "FROM showings s JOIN theaters t ON t.id=s.theater_id WHERE s.id=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_int(stmt, 1, showing_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE ? ticket_error("NOT_FOUND", "Showing not found.") : ticket_db_error(db);
    }
    int rows = sqlite3_column_int(stmt, 0);
    int columns = sqlite3_column_int(stmt, 1);
    int changed = expected.theater_id &&
        (expected.theater_id != sqlite3_column_int(stmt, 2) ||
         strcmp(expected.starts_at, (const char *)sqlite3_column_text(stmt, 3)));
    sqlite3_finalize(stmt);
    if (changed)
        return ticket_error("SHOWING_CHANGED", "The showing changed. Open the seat map again before booking.");
    for (size_t i = 0; i < count; i++) {
        if (seats[i][0] - 'A' >= rows || seat_column(seats[i]) > columns)
            return ticket_invalid("At least one seat is outside the theater layout.");
    }

    if (sqlite3_prepare_v2(db, "INSERT INTO bookings(user_id,showing_id) VALUES(?,?)",
                           -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);
    sqlite3_bind_int(stmt, 1, session->user_id);
    sqlite3_bind_int(stmt, 2, showing_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return ticket_db_error(db);

    sqlite3_int64 booking_id = sqlite3_last_insert_rowid(db);
    if (sqlite3_prepare_v2(db,
            "INSERT INTO booked_seats(booking_id,showing_id,seat) VALUES(?,?,?) "
            "ON CONFLICT(showing_id,seat) DO NOTHING", -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    TicketResult result = {0};
    sqlite3_bind_int64(stmt, 1, booking_id);
    sqlite3_bind_int(stmt, 2, showing_id);
    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_text(stmt, 3, seats[i], -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            result = ticket_db_error(db);
            break;
        }
        if (sqlite3_changes(db) == 0) {
            char message[64];
            snprintf(message, sizeof(message), "Seat %s is already booked.", seats[i]);
            result = ticket_error("SEAT_UNAVAILABLE", message);
            break;
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    return result.code ? result : ticket_created("booking_id", (int)booking_id);
}

TicketResult ticket_book(TicketSession *session, struct json_object *data)
{
    int showing_id = ticket_id(ticket_field(data, "showing_id"));
    struct json_object *selection = ticket_field(data, "seats");
    if (!showing_id || !json_object_is_type(selection, json_type_array))
        return ticket_invalid("Expected showing_id and a seat array.");

    ShowingSnapshot expected = {0};
    struct json_object *snapshot;
    if (json_object_object_get_ex(data, "expected_showing", &snapshot)) {
        expected.theater_id = ticket_id(ticket_field(snapshot, "theater_id"));
        expected.starts_at = ticket_text(ticket_field(snapshot, "starts_at"));
        if (!expected.theater_id || !ticket_valid_start(expected.starts_at))
            return ticket_invalid("Expected a theater_id and starts_at in expected_showing.");
    }

    size_t count = json_object_array_length(selection);
    if (count < 1 || count > TICKET_BATCH_MAX)
        return ticket_invalid("Choose 1–10 distinct seats, for example A1, A2.");

    const char *seats[TICKET_BATCH_MAX];
    for (size_t i = 0; i < count; i++) {
        seats[i] = ticket_text(json_object_array_get_idx(selection, i));
        if (!seat_column(seats[i]))
            return ticket_invalid("Seat labels must start with a row letter followed by a seat number.");
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(seats[i], seats[j]))
                return ticket_invalid("Select each seat only once.");
        }
    }

    sqlite3 *db = session->db;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return ticket_db_error(db);
    return ticket_commit(db, create_booking(session, showing_id, seats, count, expected));
}
