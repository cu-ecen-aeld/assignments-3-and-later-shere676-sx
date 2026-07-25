#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdbool.h>

#define PORT             "9000"
#define BACKLOG          10
#define DATA_FILE        "/var/tmp/aesdsocketdata"
#define RECV_CHUNK_SIZE  1024
#define TIMESTAMP_PERIOD_SEC 10

/* Global state so the signal handler can trigger a clean shutdown */
static volatile sig_atomic_t g_exit_requested = 0;
static int g_listen_fd = -1;

/* Serializes all writes (packet data + timestamp) to DATA_FILE */
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- Thread bookkeeping (singly linked list) ------------------------- */

struct thread_node {
    pthread_t thread_id;
    int client_fd;
    char ip_str[INET6_ADDRSTRLEN];
    volatile bool thread_complete; /* set true by the thread just before it exits */
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list_head, thread_node);
static struct thread_list_head g_thread_list = SLIST_HEAD_INITIALIZER(g_thread_list);
/* Protects g_thread_list itself (insert/remove/iterate) */
static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_timer_thread;
static bool g_timer_thread_started = false;

/* --- Signal handling --------------------------------------------------- */

static void signal_handler(int signo)
{
    (void)signo;
    g_exit_requested = 1;

    /* Wake up a blocking accept()/select() in the main loop */
    if (g_listen_fd != -1) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
}

static int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGINT) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        syslog(LOG_ERR, "sigaction(SIGTERM) failed: %s", strerror(errno));
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);

    return 0;
}

/* --- Socket setup ------------------------------------------------------- */

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

/* --- Daemonize ----------------------------------------------------------
 * Called AFTER the socket has already been successfully bound+listening,
 * so bind failures are reported before detaching. All other setup (mutex
 * init, timer thread, thread list) happens in the caller, in the child,
 * after this returns 0.
 */
static int daemonize(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        syslog(LOG_ERR, "fork() failed: %s", strerror(errno));
        return -1;
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS); /* parent exits */
    }

    if (setsid() == -1) {
        syslog(LOG_ERR, "setsid() failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") == -1) {
        syslog(LOG_ERR, "chdir() failed: %s", strerror(errno));
        return -1;
    }

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

/* --- Data file helpers ---------------------------------------------------
 * NOTE: callers must hold g_file_mutex before calling these.
 */

static int append_to_data_file_locked(const char *buf, size_t len)
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

static int send_data_file_locked(int client_fd)
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

static void remove_data_file(void)
{
    if (unlink(DATA_FILE) == -1 && errno != ENOENT) {
        syslog(LOG_ERR, "unlink(%s) failed: %s", DATA_FILE, strerror(errno));
    }
}

/* --- Timer thread ---------------------------------------------------------
 * Wakes once a second to check the exit flag (for prompt shutdown), and
 * every TIMESTAMP_PERIOD_SEC seconds appends an RFC 2822 timestamp line,
 * taking g_file_mutex for the duration of the write so it can never
 * interleave with a connection thread's packet write.
 */
static void *timer_thread_func(void *arg)
{
    (void)arg;
    int seconds_waited = 0;

    while (!g_exit_requested) {
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&ts, NULL);

        if (g_exit_requested) {
            break;
        }

        seconds_waited++;
        if (seconds_waited < TIMESTAMP_PERIOD_SEC) {
            continue;
        }
        seconds_waited = 0;

        time_t now = time(NULL);
        struct tm tm_now;
        if (localtime_r(&now, &tm_now) == NULL) {
            syslog(LOG_ERR, "localtime_r() failed");
            continue;
        }

        char time_buf[128];
        /* RFC 2822 compliant format, e.g. "Mon, 07 Aug 2006 12:34:56 -0600" */
        size_t tlen = strftime(time_buf, sizeof(time_buf), "%a, %d %b %Y %H:%M:%S %z", &tm_now);
        if (tlen == 0) {
            syslog(LOG_ERR, "strftime() failed");
            continue;
        }

        char line_buf[160];
        int llen = snprintf(line_buf, sizeof(line_buf), "timestamp:%s\n", time_buf);
        if (llen < 0) {
            continue;
        }

        pthread_mutex_lock(&g_file_mutex);
        append_to_data_file_locked(line_buf, (size_t)llen);
        pthread_mutex_unlock(&g_file_mutex);
    }

    return NULL;
}

/* --- Connection thread ----------------------------------------------------
 * Handles a single client connection: receives data, appending each
 * newline-terminated packet to DATA_FILE (mutex protected) and echoing
 * the file contents back after each packet. Exits when the peer closes
 * the connection, an error occurs on send/recv, or shutdown is requested
 * (which causes the socket to be shut down out from under us, unblocking
 * recv()).
 */
static void *connection_thread_func(void *arg)
{
    struct thread_node *node = (struct thread_node *)arg;
    int client_fd = node->client_fd;

    char *packet_buf = NULL;
    size_t packet_len = 0;
    size_t packet_cap = 0;
    char recv_buf[RECV_CHUNK_SIZE];

    while (1) {
        ssize_t n = recv(client_fd, recv_buf, sizeof(recv_buf), 0);
        if (n == 0) {
            break; /* peer closed */
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* error (including being unblocked by shutdown()) */
        }

        size_t needed = packet_len + (size_t)n;
        if (needed > packet_cap) {
            size_t new_cap = packet_cap == 0 ? RECV_CHUNK_SIZE : packet_cap;
            while (new_cap < needed) {
                new_cap *= 2;
            }
            char *new_buf = realloc(packet_buf, new_cap);
            if (new_buf == NULL) {
                syslog(LOG_ERR, "malloc/realloc failed for packet buffer, discarding packet");
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

        size_t start = 0;
        for (size_t i = 0; i < packet_len; i++) {
            if (packet_buf[i] == '\n') {
                size_t pkt_size = (i - start) + 1;

                pthread_mutex_lock(&g_file_mutex);
                if (append_to_data_file_locked(packet_buf + start, pkt_size) == 0) {
                    send_data_file_locked(client_fd);
                }
                pthread_mutex_unlock(&g_file_mutex);

                start = i + 1;
            }
        }

        if (start > 0) {
            size_t remaining = packet_len - start;
            memmove(packet_buf, packet_buf + start, remaining);
            packet_len = remaining;
        }
    }

    free(packet_buf);

    syslog(LOG_INFO, "Closed connection from %s", node->ip_str);
    close(client_fd);

    /* Signal completion; main thread will pthread_join() and free the node */
    node->thread_complete = true;

    return NULL;
}

/*
 * Walk the thread list and pthread_join() + remove any node whose thread
 * has finished. Safe to call opportunistically from the main loop.
 */
static void reap_completed_threads(void)
{
    pthread_mutex_lock(&g_list_mutex);

    struct thread_node *node = SLIST_FIRST(&g_thread_list);
    while (node != NULL) {
        struct thread_node *next = SLIST_NEXT(node, entries);
        if (node->thread_complete) {
            pthread_join(node->thread_id, NULL);
            SLIST_REMOVE(&g_thread_list, node, thread_node, entries);
            free(node);
        }
        node = next;
    }

    pthread_mutex_unlock(&g_list_mutex);
}

/* Join and free every remaining node unconditionally (used at shutdown) */
static void join_all_threads(void)
{
    pthread_mutex_lock(&g_list_mutex);

    struct thread_node *node;
    while ((node = SLIST_FIRST(&g_thread_list)) != NULL) {
        SLIST_REMOVE_HEAD(&g_thread_list, entries);
        pthread_mutex_unlock(&g_list_mutex);

        /* Unblock the thread's recv() if it's still waiting */
        shutdown(node->client_fd, SHUT_RDWR);
        pthread_join(node->thread_id, NULL);
        free(node);

        pthread_mutex_lock(&g_list_mutex);
    }

    pthread_mutex_unlock(&g_list_mutex);
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

    /*
     * Start the timer thread here, in the process that will actually run
     * (the daemonized child if -d was given, or this same process
     * otherwise) -- never in a parent that's about to exit().
     */
    if (pthread_create(&g_timer_thread, NULL, timer_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "pthread_create() for timer thread failed: %s", strerror(errno));
        close(g_listen_fd);
        closelog();
        return -1;
    }
    g_timer_thread_started = true;

    int ret = 0;

    while (!g_exit_requested) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_listen_fd, &readfds);

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        int sel = select(g_listen_fd + 1, &readfds, NULL, NULL, &tv);

        if (sel < 0) {
            if (errno == EINTR) {
                reap_completed_threads();
                continue;
            }
            syslog(LOG_ERR, "select() failed: %s", strerror(errno));
            break;
        }

        /* Opportunistically clean up finished connection threads every pass */
        reap_completed_threads();

        if (sel == 0) {
            continue; /* timeout: re-check g_exit_requested */
        }

        if (!FD_ISSET(g_listen_fd, &readfds)) {
            continue;
        }

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

        struct thread_node *node = calloc(1, sizeof(struct thread_node));
        if (node == NULL) {
            syslog(LOG_ERR, "calloc() failed for thread node, dropping connection");
            close(client_fd);
            continue;
        }
        node->client_fd = client_fd;
        node->thread_complete = false;

        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &s->sin_addr, node->ip_str, sizeof(node->ip_str));
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, node->ip_str, sizeof(node->ip_str));
        }

        syslog(LOG_INFO, "Accepted connection from %s", node->ip_str);

        if (pthread_create(&node->thread_id, NULL, connection_thread_func, node) != 0) {
            syslog(LOG_ERR, "pthread_create() failed: %s", strerror(errno));
            close(client_fd);
            free(node);
            continue;
        }

        pthread_mutex_lock(&g_list_mutex);
        SLIST_INSERT_HEAD(&g_thread_list, node, entries);
        pthread_mutex_unlock(&g_list_mutex);
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    if (g_listen_fd != -1) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    /* Request exit from, and wait for, every outstanding connection thread */
    join_all_threads();

    /* Timer thread checks g_exit_requested itself; just join it */
    if (g_timer_thread_started) {
        pthread_join(g_timer_thread, NULL);
    }

    remove_data_file();

    pthread_mutex_destroy(&g_file_mutex);
    pthread_mutex_destroy(&g_list_mutex);

    closelog();
    return ret;
}
