/*
 * client.c — Client ifnetshow
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 5050
#define BUF_SIZE 8192

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage:\n");
        printf("  ifnetshow -n <addr> -a\n");
        printf("  ifnetshow -n <addr> -i <ifname>\n");
        return EXIT_FAILURE;
    }

    const char *addr = NULL;
    int show_all = 0;
    const char *ifname = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) addr = argv[++i];
        else if (strcmp(argv[i], "-a") == 0) show_all = 1;
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) ifname = argv[++i];
    }

    if (!addr || (!show_all && !ifname)) {
        printf("Arguments invalides.\n");
        return EXIT_FAILURE;
    }

    int sockfd;
    struct sockaddr_in serv;
    char buffer[BUF_SIZE];

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        exit(EXIT_FAILURE);
    }

    serv.sin_family = AF_INET;
    serv.sin_port = htons(PORT);
    if (inet_pton(AF_INET, addr, &serv.sin_addr) <= 0) {
        exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        exit(EXIT_FAILURE);
    }

    char cmd[128];
    if (show_all)
        strcpy(cmd, "ALL\n");
    else {
        snprintf(cmd, sizeof(cmd), "IF %s\n", ifname);
    }

    send(sockfd, cmd, strlen(cmd), 0);

    memset(buffer, 0, sizeof(buffer));
    ssize_t n = read(sockfd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        printf("%s\n", buffer);
    } else {
        printf("Aucune donnée reçue.\n");
    }

    close(sockfd);
    return 0;
}
