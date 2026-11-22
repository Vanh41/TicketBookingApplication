#ifndef CLIENT_H
#define CLIENT_H

/* Runs the terminal session. Returns 0 on normal exit and -1 on I/O or protocol error. */
int client_main_loop(int sockfd);

#endif
