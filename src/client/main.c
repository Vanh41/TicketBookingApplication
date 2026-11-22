#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client.h"

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    // 1. Validate command-line arguments
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <Server_IP> <Port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 3636\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *server_ip = argv[1];
    char *end;
    long port = strtol(argv[2], &end, 10);

    // Validate port range
    if (*end || port <= 0 || port > 65535)
    {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be between 1-65535.\n", argv[2]);
        return EXIT_FAILURE;
    }

    printf("Connecting to %s:%ld ...\n", server_ip, port);

    // 2. Create TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 3. Setup server address structure
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IP address from text to binary form
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Error: Invalid address or Address not supported: %s\n", server_ip);
        close(sockfd);
        return EXIT_FAILURE;
    }

    // 4. Connect to server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Connection failed");
        fprintf(stderr, "Could not connect to %s:%ld\n", server_ip, port);
        fprintf(stderr, "Please ensure the server is running.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("✓ Connected to server successfully!\n");
    printf("═══════════════════════════════════════\n\n");

    int status = client_main_loop(sockfd);
    close(sockfd);
    if (status != 0)
        return EXIT_FAILURE;

    printf("\nConnection closed. Goodbye!\n");

    return EXIT_SUCCESS;
}
