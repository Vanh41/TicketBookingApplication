#include "ticket_server.h"

#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_int active_clients;

static void *client_worker(void *argument)
{
    TicketStream stream = {.fd = *(int *)argument};
    free(argument);

    TicketSession session = {0};
    int fd = stream.fd;
    if (ticket_open_db(&session.db) == 0) {
        char line[TICKET_MESSAGE_MAX];
        size_t length;
        int status;
        while ((status = ticket_read_line(&stream, line, &length)) != TICKET_READ_CLOSED &&
               status != TICKET_READ_FAILED) {
            int sent = status == TICKET_READ_TOO_LARGE
                ? ticket_reject_large_request(fd)
                : ticket_handle_request(&session, fd, line, length);
            if (sent != 0)
                break;
        }
        sqlite3_close(session.db);
    }

    char message[64];
    snprintf(message, sizeof(message), "DISCONNECT fd=%d", fd);
    log_activity(message);
    close(fd);
    atomic_fetch_sub(&active_clients, 1);
    return NULL;
}

static int serve(int port)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, MAX_CLIENTS) != 0) {
        perror("Could not start TCP listener");
        close(listener);
        return 1;
    }

    char message[96];
    snprintf(message, sizeof(message), "Ticket server listening on TCP port %d", port);
    log_activity(message);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_length);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        if (atomic_load(&active_clients) >= MAX_CLIENTS) {
            close(fd);
            continue;
        }

        int *argument = malloc(sizeof(*argument));
        if (!argument) {
            close(fd);
            continue;
        }
        *argument = fd;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        snprintf(message, sizeof(message), "CONNECT fd=%d peer=%s", fd, ip);
        log_activity(message);

        pthread_t thread;
        atomic_fetch_add(&active_clients, 1);
        int rc = pthread_create(&thread, NULL, client_worker, argument);
        if (rc != 0) {
            fprintf(stderr, "Could not start client thread: %s\n", strerror(rc));
            atomic_fetch_sub(&active_clients, 1);
            free(argument);
            close(fd);
            continue;
        }
        pthread_detach(thread);
    }

    close(listener);
    return 1;
}

int main(int argc, char **argv)
{
    int setup = argc == 3 && !strcmp(argv[1], "--init-admin");
    long port = SERVER_PORT;
    if (argc == 2) {
        char *end;
        port = strtol(argv[1], &end, 10);
        if (*end || port < 1 || port > 65535)
            port = 0;
    } else if (argc != 1 && !setup) {
        port = 0;
    }
    if (!port) {
        fprintf(stderr, "Usage: %s [port] | --init-admin USERNAME\n", argv[0]);
        return 1;
    }

    if (!setlocale(LC_CTYPE, "C.UTF-8") && !setlocale(LC_CTYPE, "en_US.UTF-8"))
        setlocale(LC_CTYPE, "");
    signal(SIGPIPE, SIG_IGN);
    umask(0077);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (mkdir(DATA_DIR, 0700) != 0 && errno != EEXIST) {
        perror("Could not create data directory");
        return 1;
    }

    sqlite3 *db;
    if (ticket_open_db(&db) != 0)
        return 1;
    int status = ticket_init_db(db);
    if (status == 0 && setup) {
        const char *password = getenv("TICKET_ADMIN_PASSWORD");
        if (!password)
            password = getpass("Initial admin password: ");
        status = ticket_bootstrap_admin(db, argv[2], password);
        if (status == 0)
            puts("An admin account is configured.");
    }
    sqlite3_close(db);
    if (status || setup)
        return status != 0;
    return serve((int)port);
}
