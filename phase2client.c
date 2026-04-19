#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080

void client_shell(int sockfd) {
    char buffer[1024];
    while (1) {
        printf("shel> ");

        // Get command from user
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) break;

        // Send command to server
        if (send(sockfd, buffer, strlen(buffer), 0) < 0) {
            perror("send");
            break;
        }

        // Read command output from server
        int n = read(sockfd, buffer, sizeof(buffer) - 1);
        if (n < 0) {
            perror("read");
            break;
        }
        if (n == 0) {
            // Server closed the connection
            printf("Server closed the connection\n");
            break;
        }
        buffer[n] = '\0';

        // Display command output
        printf("%s", buffer);
    }
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr;

    // Create socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    // Define server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    // Interact with the server
    client_shell(sockfd);

    // Close the socket
    close(sockfd);
    return 0;
}