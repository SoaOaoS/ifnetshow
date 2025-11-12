/*
 * ifnetshow agent serveur
 *
 * Ce programme expose les fonctions de ifshow sur le réseau. 
 * Il utilise dup2 pour capturer la sortie de stdout dans un buffer
 * et l'envoyer au client TCP.
 */

#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>

#define PORT 5050
#define BUF_SIZE 8192

/*************************
 * Fonctions ifshow exactes
 *************************/

static int addr_to_string(const struct sockaddr *sa, char *buf, size_t buflen) {
    if (!sa || !buf || buflen == 0) return -1;
    switch (sa->sa_family) {
        case AF_INET: {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
            return inet_ntop(AF_INET, &sin->sin_addr, buf, (socklen_t)buflen) ? 0 : -1;
        }
        case AF_INET6: {
            const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
            return inet_ntop(AF_INET6, &sin6->sin6_addr, buf, (socklen_t)buflen) ? 0 : -1;
        }
        default:
            return -1;
    }
}

static int count_prefix_length(const struct sockaddr *netmask) {
    if (!netmask) return -1;
    if (netmask->sa_family == AF_INET) {
        const struct sockaddr_in *nm4 = (const struct sockaddr_in *)netmask;
        uint32_t m = ntohl(nm4->sin_addr.s_addr);
        int count = 0;
        for (int i = 31; i >= 0; --i) {
            if ((m >> i) & 1U) count++; else break;
        }
        return count;
    } else if (netmask->sa_family == AF_INET6) {
        const struct sockaddr_in6 *nm6 = (const struct sockaddr_in6 *)netmask;
        int count = 0;
        for (int i = 0; i < 16; ++i) {
            unsigned char b = nm6->sin6_addr.s6_addr[i];
            for (int bit = 7; bit >= 0; --bit) {
                if ((b >> bit) & 1U) count++; else return count;
            }
        }
        return count;
    }
    return -1;
}

void help() {
    printf("Usage:\n");
    printf("  ifshow -a                     # Show all interfaces\n");
    printf("  ifshow -i <interface_name>    # Show specific interface\n");
    printf("\nExamples:\n");
    printf("  ifshow -a\n");
    printf("  ifshow -i eth0\n");
    printf("\nNotes:\n");
    printf("  Addresses include netmask as address/prefix.\n");
    printf("  IPv4 also shows dotted mask in parentheses.\n\n");
}

static void print_interface_header(const char *ifname) {
    if (ifname && *ifname) printf("%s:\n", ifname);
}

static void print_address_bullet(const struct sockaddr *addr, const struct sockaddr *netmask) {
    if (!addr) return;
    char addr_str[NI_MAXHOST] = {0};
    char mask_str[NI_MAXHOST] = {0};
    int family = addr->sa_family;
    if (addr_to_string(addr, addr_str, sizeof(addr_str)) != 0) return;
    int prefix = count_prefix_length(netmask);
    if (family == AF_INET && netmask && addr_to_string(netmask, mask_str, sizeof(mask_str)) == 0 && prefix >= 0) {
        printf(" - %s/%d (%s)\n", addr_str, prefix, mask_str);
    } else if (prefix >= 0) {
        printf(" - %s/%d\n", addr_str, prefix);
    } else {
        printf(" - %s\n", addr_str);
    }
}

static void show_all_interfaces(void) {
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    const int MAX_IFS = 256;
    const char *names[MAX_IFS];
    int name_count = 0;

    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = (*ifa).ifa_next) {
        if (!ifa || !(*ifa).ifa_addr) continue;
        int family = (*ifa).ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        int seen = 0;
        for (int i = 0; i < name_count; ++i) {
            if (strcmp(names[i], (*ifa).ifa_name) == 0) { seen = 1; break; }
        }
        if (!seen && name_count < MAX_IFS) names[name_count++] = (*ifa).ifa_name;
    }

    for (int i = 0; i < name_count; ++i) {
        const char *ifname = names[i];
        print_interface_header(ifname);
        for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = (*ifa).ifa_next) {
            if (!ifa || !(*ifa).ifa_addr) continue;
            if (strcmp((*ifa).ifa_name, ifname) != 0) continue;
            int family = (*ifa).ifa_addr->sa_family;
            if (family == AF_INET || family == AF_INET6) {
                print_address_bullet((*ifa).ifa_addr, (*ifa).ifa_netmask);
            }
        }
        printf("\n");
    }

    freeifaddrs(ifaddr);
}

static void show_single_interface(const char *target_ifname) {
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }

    int found = 0;
    print_interface_header(target_ifname);
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = (*ifa).ifa_next) {
        if (!ifa || !(*ifa).ifa_addr) continue;
        if (strcmp((*ifa).ifa_name, target_ifname) != 0) continue;
        int family = (*ifa).ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) {
            print_address_bullet((*ifa).ifa_addr, (*ifa).ifa_netmask);
            found = 1;
        }
    }

    freeifaddrs(ifaddr);
    if (!found) printf("Interface '%s' not found or has no IP.\n", target_ifname);
}

/**************************************
 * Capture stdout via dup2 pour serveur
 **************************************/
static void capture_output(void (*func)(const char *), const char *arg, char *buf, size_t buflen) {
    int pipefd[2];
    if (pipe(pipefd) < 0) { snprintf(buf, buflen, "Erreur interne\n"); return; }

    // Sauvegarder stdout actuel
    int stdout_save = dup(fileno(stdout));
    if (stdout_save < 0) { snprintf(buf, buflen, "Erreur interne\n"); close(pipefd[0]); close(pipefd[1]); return; }

    // Rediriger stdout vers le pipe
    dup2(pipefd[1], fileno(stdout));
    close(pipefd[1]);

    // Appeler la fonction ifshow
    if (func) func(arg);
    else show_all_interfaces();

    // Restaurer stdout
    fflush(stdout);
    dup2(stdout_save, fileno(stdout));
    close(stdout_save);

    // Lire le contenu du pipe dans le buffer
    ssize_t n = read(pipefd[0], buf, buflen - 1);
    if (n >= 0) buf[n] = 0;
    else buf[0] = 0;
    close(pipefd[0]);
}

/*********************
 * Serveur TCP
 *********************/
int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); exit(EXIT_FAILURE); }
    if (listen(server_fd, 5) < 0) { perror("listen"); exit(EXIT_FAILURE); }

    printf("Serveur ifnetshow en écoute sur le port %d...\n", PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli, &cli_len);
        if (client_fd < 0) continue;

        char buf[1024] = {0};
        read(client_fd, buf, sizeof(buf) - 1);
        buf[strcspn(buf, "\r\n")] = 0;

        char result[BUF_SIZE] = {0};
        if (strncmp(buf, "ALL", 3) == 0)
            capture_output(NULL, NULL, result, sizeof(result));
        else if (strncmp(buf, "IF ", 3) == 0)
            capture_output(show_single_interface, buf + 3, result, sizeof(result));
        else
            snprintf(result, sizeof(result), "Commande invalide\n");

        send(client_fd, result, strlen(result), 0);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
