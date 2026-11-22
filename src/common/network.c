#include "network.h"

#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>

int send_all(int sockfd, const void *buffer, size_t len)
{
    const char *bytes = buffer;
    size_t sent = 0;

    while (sent < len) {
        ssize_t count = send(sockfd, bytes + sent, len - sent, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (count < 0)
                perror("send");
            return -1;
        }
        sent += (size_t)count;
    }

    return 0;
}
