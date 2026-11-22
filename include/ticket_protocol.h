#ifndef TICKET_PROTOCOL_H
#define TICKET_PROTOCOL_H

#include <json-c/json.h>

#define TICKET_MESSAGE_MAX 4096  /* Includes the terminating LF. */
#define TICKET_BATCH_MAX 10

typedef struct {
    int fd;
    char pending[TICKET_MESSAGE_MAX];
    size_t offset;
    size_t length;
} TicketStream;

enum {
    TICKET_READ_CLOSED = 0,
    TICKET_READ_OK = 1,
    TICKET_READ_TOO_LARGE = -1,
    TICKET_READ_FAILED = -2
};

int ticket_read_line(TicketStream *stream, char line[TICKET_MESSAGE_MAX], size_t *length);
int ticket_send_json(int fd, struct json_object *object);
struct json_object *ticket_parse_json(const char *line, size_t length);
struct json_object *ticket_field(struct json_object *object, const char *name);
const char *ticket_text(struct json_object *value);
int ticket_id(struct json_object *value);

#endif
