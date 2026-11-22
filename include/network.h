#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>

/* Sends all len bytes on a TCP connection. Returns 0 on success, -1 on failure. */
int send_all(int sockfd, const void *buffer, size_t len);

#endif
