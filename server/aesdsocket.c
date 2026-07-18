/*
 * aesdsocket.c
 *
 * A stream socket server that:
 *  - Binds to port 9000
 *  - Accepts connections in a loop
 *  - Receives newline-terminated packets and appends them to
 *    /var/tmp/aesdsocketdata
 *  - After each complete packet, sends the full contents of the data file
 *    back to the client
 *  - Logs connection open/close events (with client IP) to syslog
 *  - Handles SIGINT/SIGTERM for graceful shutdown, deleting the data file
 *  - Supports a -d option to run as a daemon (forking only after a
 *    successful bind to port 9000)
 *
 * Build with the accompanying Makefile (supports cross compilation via
 * CC/CROSS_COMPILE environment variables).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdbool.h>

#define PORT            "9000"
#define BACKLOG         10
#define DATA_FILE       "/var/tmp/aesdsocketdata"
#define RECV_CHUNK_SIZE 1024

/* Global state so the signal handler can trigger a clean shutdown */
static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;
static int g_client_fd = -1;

static void signal_handler(int signo)
{
    (void)signo;
    g_exit_requested = 1;

    /*
     * Shut down any sockets that might currently be blocking in
     * accept()/recv()/send() so the main loop can wake up and notice
     * the exit request. These calls are async-signal-safe.
     */
    if (g_listen_fd != -1) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
    if (g_client_fd != -1) {
        shutdown(g_client_fd, SHUT_RDWR);
    }
}

static int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: we want blocking calls interrupted */

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGINT) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGTERM) failed: %s", strerror(errno));
        return -1;
    }
    /* Ignore SIGPIPE so writes to a closed socket don't kill us */
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

/*
 * Create, bind (and start listening on) a stream socket for PORT.
 * Returns the listening socket fd on success, -1 on failure.
 */
static int create_and_bind_socket(void)
{
    struct addrinfo hints, *servinfo = NULL, *p;
    int rv;
    int sockfd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    rv = getaddrinfo(NULL, PORT, &hints, &servinfo);
    if (rv != 0) {
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(rv));
        return -1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) {
            syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            syslog(LOG_ERR, "setsockopt() failed: %s", strerror(errno));
            close(sockfd);
            sockfd = -1;
            continue;
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            syslog(LOG_ERR, "bind() failed: %s", strerror(errno));
            close(sockfd);
            sockfd = -1;
            continue;
        }

        break; /* success */
    }

    freeaddrinfo(servinfo);

    if (sockfd == -1 || p == NULL) {
        syslog(LOG_ERR, "Failed to bind to port %s", PORT);
        return -1;
    }

    if (listen(sockfd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen() failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/*
 * Daemonize the process. Should be called AFTER the socket has already
 * been successfully bound (and is listening), so that any bind failure
 * is reported to the (still attached) parent/foreground process before
 * detaching.
 */
static int daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        syslog(LOG_ERR, "fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        /* Parent exits, letting the child continue as the daemon */
        exit(EXIT_SUCCESS);
    }

    /* Child continues here */
    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid() failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") == -1) {
        syslog(LOG_ERR, "chdir() failed: %s", strerror(errno));
        return -1;
    }

    /* Redirect standard file descriptors to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull == -1) {
        syslog(LOG_ERR, "open(/dev/null) failed: %s", strerror(errno));
        return -1;
    }
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO) {
        close(devnull);
    }

    return 0;
}

/*
 * Append the buffer of length len to DATA_FILE.
 * Returns 0 on success, -1 on failure.
 */
static int append_to_data_file(const char *buf, size_t len)
{
    int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        syslog(LOG_ERR, "open(%s) failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "write(%s) failed: %s", DATA_FILE, strerror(errno));
            close(fd);
            return -1;
        }
        written += (size_t)n;
    }

    close(fd);
    return 0;
}

/*
 * Send the entire contents of DATA_FILE to the client socket.
 * Returns 0 on success, -1 on failure.
 */
static int send_data_file(int client_fd)
{
    int fd = open(DATA_FILE, O_RDONLY);
    if (fd == -1) {
        syslog(LOG_ERR, "open(%s) failed: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    char buf[RECV_CHUNK_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t sent_total = 0;
        while (sent_total < n) {
            ssize_t s = send(client_fd, buf + sent_total, (size_t)(n - sent_total), 0);
            if (s == -1) {
                if (errno == EINTR) {
                    continue;
                }
                syslog(LOG_ERR, "send() failed: %s", strerror(errno));
                close(fd);
                return -1;
            }
            sent_total += s;
        }
    }

    if (n == -1) {
        syslog(LOG_ERR, "read(%s) failed: %s", DATA_FILE, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

/*
 * Handle a single client connection: receive data, appending each
 * newline-terminated packet to DATA_FILE and echoing the file contents
 * back after each packet. Returns when the connection is closed by the
 * peer, an error occurs, or a shutdown has been requested.
 */
static void handle_client(int client_fd)
{
    char *packet_buf = NULL;
    size_t packet_len = 0;   /* bytes currently stored in packet_buf */
    size_t packet_cap = 0;   /* allocated capacity of packet_buf */
    char recv_buf[RECV_CHUNK_SIZE];

    while (!g_exit_requested) {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n == 0) {
            /* Connection closed by peer */
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            syslog(LOG_ERR, "recv() failed: %s", strerror(errno));
            break;
        }

        /* Append newly received bytes into the growable packet buffer */
        size_t needed = packet_len + (size_t)n;
        if (needed > packet_cap) {
            size_t new_cap = packet_cap == 0 ? RECV_CHUNK_SIZE : packet_cap;
            while (new_cap < needed) {
                new_cap *= 2;
            }
            char *new_buf = realloc(packet_buf, new_cap);
            if (new_buf == NULL) {
                syslog(LOG_ERR, "malloc/realloc failed for packet buffer, discarding packet");
                /* Discard what we have so far and keep reading */
                free(packet_buf);
                packet_buf = NULL;
                packet_len = 0;
                packet_cap = 0;
                continue;
            }
            packet_buf = new_buf;
            packet_cap = new_cap;
        }
        memcpy(packet_buf + packet_len, recv_buf, (size_t)n);
        packet_len += (size_t)n;

        /*
         * Process every complete (newline-terminated) packet currently
         * present in the buffer.
         */
        size_t start = 0;
        for (size_t i = 0; i < packet_len; i++) {
            if (packet_buf[i] == '\n') {
                size_t pkt_size = (i - start) + 1; /* include the newline */

                if (append_to_data_file(packet_buf + start, pkt_size) == 0) {
                    send_data_file(client_fd);
                }

                start = i + 1;
            }
        }

        /* Shift any leftover (incomplete) data to the front of the buffer */
        if (start > 0) {
            size_t remaining = packet_len - start;
            memmove(packet_buf, packet_buf + start, remaining);
            packet_len = remaining;
        }
    }

    free(packet_buf);
}

static void remove_data_file(void)
{
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_ERR, "unlink(%s) failed: %s", DATA_FILE, strerror(errno));
    }
}

int main(int argc, char *argv[])
{
    bool run_as_daemon = false;
    int opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
            case 'd':
                run_as_daemon = true;
                break;
            default:
                fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
                return -1;
        }
    }

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);

    if (setup_signal_handlers() != 0) {
        closelog();
        return -1;
    }

    g_listen_fd = create_and_bind_socket();
    if (g_listen_fd == -1) {
        closelog();
        return -1;
    }

    if (run_as_daemon) {
        if (daemonize() != 0) {
            close(g_listen_fd);
            closelog();
            return -1;
        }
    }

    int ret = 0;

    while (!g_exit_requested) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (errno == EINTR || g_exit_requested) {
                break;
            }
            syslog(LOG_ERR, "accept() failed: %s", strerror(errno));
            continue;
        }

        g_client_fd = client_fd;

        char ip_str[INET6_ADDRSTRLEN] = {0};
        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &s->sin_addr, ip_str, sizeof(ip_str));
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, ip_str, sizeof(ip_str));
        }

        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        handle_client(client_fd);

        syslog(LOG_INFO, "Closed connection from %s", ip_str);

        close(client_fd);
        g_client_fd = -1;
    }

    if (g_exit_requested) {
        syslog(LOG_INFO, "Caught signal, exiting");
    }

    if (g_listen_fd != -1) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    remove_data_file();

    closelog();
    return ret;
}
