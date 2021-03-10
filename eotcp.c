/*
 * eotcp.c
 *
 *  Created on: Feb 8, 2021
 *      Author: ovalentin
 */

#include <sys/socket.h>

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MTU 1600

static int quit_requested = 0;
static int tap_fd;

static int build_address(const char* addr, const char* port, struct sockaddr_in* inaddr)
{
    struct addrinfo hints = { 0 }, *addrinfo;

    hints.ai_family = AF_INET;

    if (getaddrinfo(addr, port, &hints, &addrinfo)) {
        perror("Resolve address");
        return -1;
    }

    memcpy(inaddr, addrinfo->ai_addr, sizeof(*inaddr));

    freeaddrinfo(addrinfo);
    return 0;
}

static int send_it_all(int fd, char* buffer, int length)
{
    unsigned int sent = 0;

    while (sent < length) {
        int rc;
        rc = send(fd, buffer + sent, length - sent, 0);
        if (rc < 0) {
            perror("send_it_all");
            return rc;
        }
        sent += rc;
    }
    return 0;
}

static int recv_it_all(int fd, char* buffer, int length)
{
    unsigned int recvd = 0;

    while (recvd < length) {
        int rc;
        rc = recv(fd, buffer + recvd, length - recvd, 0);
        if (rc <= 0) {
            perror("recv_it_all");
            return -1;
        }
        recvd += rc;
    }
    return 0;
}

static void pump(int fd)
{
    struct pollfd fds[2];

    fds[0].fd     = tap_fd;
    fds[0].events = POLLIN;

    fds[1].fd     = fd;
    fds[1].events = POLLIN;

    while (!quit_requested) {
        if (poll(fds, 2, -1) != -1) {
            if (fds[0].revents & POLLIN) {
                char buffer[MTU];
                int pkt_len;
                unsigned char header[4];

                pkt_len = read(tap_fd, buffer, sizeof(buffer));

                if (pkt_len < 0) {
                    perror("recv(TAP)");
                    break;
                }

                header[0] = pkt_len >> 24;
                header[1] = pkt_len >> 16;
                header[2] = pkt_len >> 8;
                header[3] = pkt_len;

                send_it_all(fd, (char*)header, sizeof(header));
                send_it_all(fd, buffer, pkt_len);
            }
            if (fds[1].revents & POLLIN) {
                char buffer[MTU];
                int pkt_len;
                unsigned char header[4];

                recv_it_all(fd, (char*)header, sizeof(header));
                pkt_len = (header[0] << 24) + (header[1] << 16) + (header[2] << 8) + header[3];

                if (pkt_len > MTU) {
                    fprintf(stderr, "packet larger than MTU (%d)\n", pkt_len);
                    break;
                }

                if (recv_it_all(fd, buffer, pkt_len) < 0)
                    break;

                write(tap_fd, buffer, pkt_len);
            }
        }
    }
}

static void server_loop(const char* addr, const char* port)
{
    struct sockaddr_in inaddr = { 0 };
    int server_fd;

    if (build_address(addr, port, &inaddr))
        return;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (bind(server_fd, (struct sockaddr*)&inaddr, sizeof(inaddr)) == -1) {
        perror("bind");
        return;
    };

    listen(server_fd, 1);

    while (!quit_requested) {
        int fd = accept(server_fd, NULL, NULL);

        pump(fd);

        close(fd);
    }
}

static void client_loop(const char* addr, const char* port)
{
    struct sockaddr_in inaddr = { 0 };

    if (build_address(addr, port, &inaddr))
        return;

    while (!quit_requested) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (connect(fd, (struct sockaddr*)&inaddr, sizeof(inaddr)) != -1)
            pump(fd);

        if (fd != -1)
            close(fd);

        sleep(1);
    }
}

static int init_tap(const char* tap_name)
{
    struct ifreq ifr = { 0 };

    tap_fd = open("/dev/net/tun", O_RDWR);

    if (tap_fd == -1) {
        perror("opening /dev/net/tun");
        return -1;
    }

    if (tap_name != NULL) {
        strncpy(ifr.ifr_name, tap_name, sizeof(ifr.ifr_name));
        ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';
    }

    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    if (ioctl(tap_fd, TUNSETIFF, (void*)&ifr) < 0) {
        perror("TUNSETIFF");
        close(tap_fd);
        return -1;
    }

    return tap_fd;
}

static void signal_handler(int sig)
{
    if (sig == SIGTERM)
        quit_requested = 1;
}

static void usage(const char* progname)
{
    fprintf(stderr, "%s [-p <port>] [-t <tap> ] { -c <server_addr> | -s <bind_addr> }\n", progname);
}

int main(int argc, char** argv)
{
    int opt;
    const char* port = "4242";
    const char* addr;
    const char* tap_name = NULL;
    enum {
        ROLE_UNDEF,
        ROLE_CLIENT,
        ROLE_SERVER,
    } role = ROLE_UNDEF;

    while ((opt = getopt(argc, argv, "c:s:p:t:")) != -1) {
        switch (opt) {
        case 'c':
            if (role != ROLE_UNDEF) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            role = ROLE_CLIENT;
            addr = optarg;
            break;
        case 's':
            if (role != ROLE_UNDEF) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            role = ROLE_SERVER;
            addr = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 't':
            tap_name = optarg;
            break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    signal(SIGTERM, &signal_handler);

    if (init_tap(tap_name) < 0)
        return EXIT_FAILURE;

    switch (role) {
    case ROLE_CLIENT:
        client_loop(addr, port);
        break;
    case ROLE_SERVER:
        server_loop(addr, port);
        break;
    default:
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
