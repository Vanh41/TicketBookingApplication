#include "ticket_server.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define SALT_BYTES 16
#define HASH_BYTES 32
#define HASH_ITERATIONS 600000

static int valid_username(const char *username)
{
    if (!username || strlen(username) < 3 || strlen(username) > 20)
        return 0;
    return username[strspn(username, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")] == '\0';
}

static int valid_password(const char *password)
{
    if (!ticket_valid_text(password, 6, 32))
        return 0;
    mbstate_t state = {0};
    size_t remaining = strlen(password);
    while (remaining) {
        wchar_t character;
        size_t length = mbrtowc(&character, password, remaining, &state);
        if (length == (size_t)-1 || length == (size_t)-2 || !length)
            return 0;
        /* Locale whitespace classes can omit these Unicode whitespace characters. */
        if (iswspace(character) || character == 0x0085 || character == 0x00a0 ||
            character == 0x2007 || character == 0x202f)
            return 0;
        password += length;
        remaining -= length;
    }
    return 1;
}

static int valid_role(const char *role)
{
    return role && (!strcmp(role, "BUYER") || !strcmp(role, "MANAGER") || !strcmp(role, "ADMIN"));
}

static int hash_password(const char *password, const unsigned char *salt, unsigned char *hash)
{
    return PKCS5_PBKDF2_HMAC(password, -1, salt, SALT_BYTES, HASH_ITERATIONS,
                            EVP_sha256(), HASH_BYTES, hash) == 1;
}

static TicketResult user_with_role(int user_id, const char *role)
{
    TicketResult result = ticket_created("user_id", user_id);
    json_object_object_add(result.data, "role", json_object_new_string(role));
    return result;
}

static TicketResult insert_user(sqlite3 *db, const char *username, const char *password,
                                const char *role)
{
    unsigned char salt[SALT_BYTES], hash[HASH_BYTES];
    if (!valid_username(username) || !valid_password(password) || !valid_role(role))
        return ticket_invalid("Expected a 3–20 character username, a 6–32 character password without spaces, and a valid role.");

    if (RAND_bytes(salt, sizeof(salt)) != 1 || !hash_password(password, salt, hash))
        return ticket_error("INTERNAL_ERROR", "Could not hash the password.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO users(username,password_hash,salt,role) VALUES(?,?,?,?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        OPENSSL_cleanse(hash, sizeof(hash));
        return ticket_db_error(db);
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, hash, sizeof(hash), SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, salt, sizeof(salt), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, role, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    OPENSSL_cleanse(hash, sizeof(hash));
    if (rc != SQLITE_DONE)
        return ticket_db_error(db);
    if (sqlite3_changes(db) == 0)
        return ticket_error("USERNAME_TAKEN", "That username already exists.");
    return ticket_created("user_id", (int)sqlite3_last_insert_rowid(db));
}

int ticket_bootstrap_admin(sqlite3 *db, const char *username, const char *password)
{
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        ticket_db_error(db);
        return -1;
    }

    sqlite3_stmt *stmt;
    TicketResult result;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE role='ADMIN' LIMIT 1",
                           -1, &stmt, NULL) != SQLITE_OK) {
        result = ticket_db_error(db);
    } else {
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_ROW)
            result = ticket_ok(NULL);
        else if (rc != SQLITE_DONE)
            result = ticket_db_error(db);
        else
            result = insert_user(db, username, password, "ADMIN");
    }

    result = ticket_commit(db, result);
    if (result.code)
        fprintf(stderr, "Admin setup: %s\n", result.message);
    json_object_put(result.data);
    return result.code ? -1 : 0;
}

TicketResult ticket_register(TicketSession *session, struct json_object *data)
{
    return insert_user(session->db, ticket_text(ticket_field(data, "username")),
                       ticket_text(ticket_field(data, "password")), "BUYER");
}

TicketResult ticket_create_user(TicketSession *session, struct json_object *data)
{
    return insert_user(session->db, ticket_text(ticket_field(data, "username")),
                       ticket_text(ticket_field(data, "password")),
                       ticket_text(ticket_field(data, "role")));
}

TicketResult ticket_login(TicketSession *session, struct json_object *data)
{
    const char *username = ticket_text(ticket_field(data, "username"));
    const char *password = ticket_text(ticket_field(data, "password"));
    if (!valid_username(username) || !valid_password(password))
        return ticket_invalid("Expected a valid username and password.");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(session->db,
            "SELECT id,password_hash,salt,role FROM users WHERE username=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(session->db);

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    unsigned char hash[HASH_BYTES];
    static const unsigned char dummy_salt[SALT_BYTES];
    TicketResult result = ticket_error("INVALID_CREDENTIALS", "Invalid username or password.");
    if (rc == SQLITE_ROW && sqlite3_column_bytes(stmt, 1) == HASH_BYTES &&
        sqlite3_column_bytes(stmt, 2) == SALT_BYTES) {
        if (!hash_password(password, sqlite3_column_blob(stmt, 2), hash)) {
            result = ticket_error("INTERNAL_ERROR", "Could not verify the password.");
        } else if (CRYPTO_memcmp(hash, sqlite3_column_blob(stmt, 1), HASH_BYTES) == 0) {
            session->user_id = sqlite3_column_int(stmt, 0);
            result = user_with_role(session->user_id, (const char *)sqlite3_column_text(stmt, 3));
        }
    } else if (rc == SQLITE_DONE) {
        hash_password(password, dummy_salt, hash);
    } else {
        result = ticket_db_error(session->db);
    }

    OPENSSL_cleanse(hash, sizeof(hash));
    sqlite3_finalize(stmt);
    return result;
}

TicketResult ticket_logout(TicketSession *session, struct json_object *data)
{
    (void)data;
    session->user_id = 0;
    return ticket_ok(NULL);
}

TicketResult ticket_set_role(TicketSession *session, struct json_object *data)
{
    int id = ticket_id(ticket_field(data, "user_id"));
    const char *role = ticket_text(ticket_field(data, "new_role"));
    if (!id || !valid_role(role))
        return ticket_invalid("Expected user_id and new_role (BUYER, MANAGER, or ADMIN).");

    sqlite3 *db = session->db;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "UPDATE users SET role=?1 WHERE id=?2 AND "
            "(role<>'ADMIN' OR ?1='ADMIN' OR EXISTS(SELECT 1 FROM users WHERE role='ADMIN' AND id<>?2))",
            -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);

    sqlite3_bind_text(stmt, 1, role, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return ticket_db_error(db);
    if (sqlite3_changes(db))
        return user_with_role(id, role);

    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE id=?", -1, &stmt, NULL) != SQLITE_OK)
        return ticket_db_error(db);
    sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW)
        return ticket_error("PERMISSION_DENIED", "At least one admin account must remain.");
    if (rc == SQLITE_DONE)
        return ticket_error("NOT_FOUND", "User not found.");
    return ticket_db_error(db);
}
