#include "ticket_protocol.h"

#include "network.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>

int ticket_read_line(TicketStream *stream, char line[TICKET_MESSAGE_MAX], size_t *length)
{
    *length = 0;
    int too_large = 0;

    for (;;) {
        if (stream->offset == stream->length) {
            ssize_t received = recv(stream->fd, stream->pending, sizeof(stream->pending), 0);
            if (received < 0 && errno == EINTR)
                continue;
            if (received < 0)
                return TICKET_READ_FAILED;
            if (received == 0)
                return TICKET_READ_CLOSED;
            stream->offset = 0;
            stream->length = (size_t)received;
        }

        const char *start = stream->pending + stream->offset;
        size_t remaining = stream->length - stream->offset;
        const char *lf = memchr(start, '\n', remaining);
        size_t chunk = lf ? (size_t)(lf - start) : remaining;
        stream->offset += chunk + (lf != NULL);

        if (!too_large && chunk < TICKET_MESSAGE_MAX - *length) {
            memcpy(line + *length, start, chunk);
            *length += chunk;
        } else {
            too_large = 1;
        }

        if (lf) {
            line[*length] = '\0';
            return too_large ? TICKET_READ_TOO_LARGE : TICKET_READ_OK;
        }
    }
}

int ticket_send_json(int fd, struct json_object *object)
{
    size_t length;
    const char *json = json_object_to_json_string_length(object, JSON_C_TO_STRING_PLAIN, &length);
    if (!json || length >= TICKET_MESSAGE_MAX)
        return -1;

    char line[TICKET_MESSAGE_MAX];
    memcpy(line, json, length);
    line[length] = '\n';
    return send_all(fd, line, length + 1);
}

struct json_object *ticket_parse_json(const char *line, size_t length)
{
    if (memchr(line, '\0', length))
        return NULL;

    struct json_tokener *parser = json_tokener_new();
    if (!parser)
        return NULL;

    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *object = json_tokener_parse_ex(parser, line, (int)length);
    if (json_tokener_get_error(parser) != json_tokener_success ||
        !json_object_is_type(object, json_type_object)) {
        json_object_put(object);
        object = NULL;
    }
    json_tokener_free(parser);
    return object;
}

struct json_object *ticket_field(struct json_object *object, const char *name)
{
    struct json_object *value = NULL;
    if (json_object_is_type(object, json_type_object))
        json_object_object_get_ex(object, name, &value);
    return value;
}

const char *ticket_text(struct json_object *value)
{
    if (!json_object_is_type(value, json_type_string))
        return NULL;

    const char *text = json_object_get_string(value);
    return strlen(text) == (size_t)json_object_get_string_len(value) ? text : NULL;
}

int ticket_id(struct json_object *value)
{
    if (!json_object_is_type(value, json_type_int))
        return 0;

    int64_t id = json_object_get_int64(value);
    return id > 0 && id <= INT_MAX ? (int)id : 0;
}
