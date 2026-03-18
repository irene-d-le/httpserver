/*
 * httpserver.c
 *
 * Multi-threaded HTTP/1.1 server using POSIX threads.
 *
 *  - One dispatcher thread accepts incoming connections and enqueues them.
 *  - A configurable worker thread pool dequeues and handles requests.
 *  - A thread-safe circular queue couples the dispatcher to workers.
 *  - An atomic, linearized audit log records every request/response.
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -pthread -o httpserver httpserver.c
 *
 * Usage:
 *   ./httpserver <port> <num_workers> [logfile]
 *
 *   port        – TCP port to listen on (e.g. 8080)
 *   num_workers – worker threads in the pool (e.g. 4)
 *   logfile     – optional path for audit log (default: audit.log)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/*  tunables  */

#define QUEUE_CAPACITY   256          /* max pending connections      */
#define RECV_BUF_SIZE    8192         /* per-request receive buffer   */
#define SEND_BUF_SIZE    65536        /* per-response send buffer     */
#define BACKLOG          128          /* listen() backlog             */
#define MAX_WORKERS      64           /* hard cap on worker count     */
#define HTTP_VERSION     "HTTP/1.1"

/*audit log*/

static int   g_logfd   = -1;          /* file descriptor, -1 = stderr */
static pthread_mutex_t g_logmtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * audit_write – atomically append one formatted log line.
 * Uses a mutex so that multi-line writes from different threads
 * never interleave, producing a linearized log.
 */
static void audit_write(const char *remote_ip, int remote_port,
                         const char *method,   const char *uri,
                         int status_code,      ssize_t bytes_sent)
{
    char line[512];
    time_t now = time(NULL);
    struct tm tm_buf;
    gmtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    int len = snprintf(line, sizeof line,
        "[%s] %s:%d \"%s %s\" %d %zd\n",
        ts, remote_ip, remote_port, method, uri, status_code, bytes_sent);

    pthread_mutex_lock(&g_logmtx);
    ssize_t _w;   /* suppress warn_unused_result */
    if (g_logfd >= 0)
        _w = write(g_logfd, line, (size_t)len);   /* atomic for len ≤ PIPE_BUF */
    else
        _w = write(STDERR_FILENO, line, (size_t)len);
    (void)_w;
    pthread_mutex_unlock(&g_logmtx);
}

/*  circular queue  */

typedef struct {
    int  fds[QUEUE_CAPACITY];   /* ring buffer of accepted fds      */
    int  head;                  /* next slot to dequeue             */
    int  tail;                  /* next slot to enqueue             */
    int  count;                 /* items currently in queue         */

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;  /* workers wait here                */
    pthread_cond_t  not_full;   /* dispatcher waits here            */

    int  shutdown;              /* set to 1 to drain and exit       */
} CircularQueue;

static void queue_init(CircularQueue *q)
{
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->lock,      NULL);
    pthread_cond_init (&q->not_empty, NULL);
    pthread_cond_init (&q->not_full,  NULL);
}

/*
 * queue_push – dispatcher calls this; blocks if queue is full.
 * Returns 0 on success, -1 if shutting down.
 */
static int queue_push(CircularQueue *q, int fd)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_CAPACITY && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->lock);

    if (q->shutdown) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    q->fds[q->tail] = fd;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/*
 * queue_pop – workers call this; blocks when queue is empty.
 * Returns a valid fd, or -1 when shutting down with empty queue.
 */
static int queue_pop(CircularQueue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);

    if (q->count == 0) {  /* shutdown and empty */
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    int fd = q->fds[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return fd;
}

/*
 * queue_shutdown – wake all blocked threads so they can exit.
 */
static void queue_shutdown(CircularQueue *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

/* HTTP helpers  */

typedef struct {
    char method[16];
    char uri[2048];
    char version[16];
} HttpRequest;

/* Parse the first (request) line; returns 0 on success */
static int parse_request_line(const char *buf, HttpRequest *req)
{
    return sscanf(buf, "%15s %2047s %15s", req->method, req->uri, req->version) == 3 ? 0 : -1;
}

static const char *status_text(int code)
{
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

/*
 * send_response – write a complete HTTP response.
 * body may be NULL (no body).  Returns bytes sent (header + body).
 */
static ssize_t send_response(int fd, int status,
                              const char *content_type,
                              const char *body, size_t body_len)
{
    char header[512];
    int hlen = snprintf(header, sizeof header,
        "%s %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        HTTP_VERSION, status, status_text(status),
        content_type ? content_type : "text/plain",
        body_len);

    ssize_t sent = 0;
    ssize_t n;

    /* send header */
    n = write(fd, header, (size_t)hlen);
    if (n < 0) return n;
    sent += n;

    /* send body */
    if (body && body_len > 0) {
        n = write(fd, body, body_len);
        if (n < 0) return sent;
        sent += n;
    }
    return sent;
}

/* request handlers  */

/*
 * handle_static_file – serve a file from disk relative to CWD.
 * Very simple: strip leading '/', reject ".." traversal.
 */
static ssize_t handle_static_file(int client_fd, const char *uri)
{
    /* sanitise path */
    const char *path = uri[0] == '/' ? uri + 1 : uri;
    if (strstr(path, "..")) {
        const char *body = "403 Forbidden\n";
        return send_response(client_fd, 400, "text/plain", body, strlen(body));
    }
    if (*path == '\0') path = "index.html";

    /* try to open the file */
    int ffd = open(path, O_RDONLY);
    if (ffd < 0) {
        const char *body = "404 Not Found\n";
        return send_response(client_fd, 404, "text/plain", body, strlen(body));
    }

    struct stat st;
    if (fstat(ffd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(ffd);
        const char *body = "404 Not Found\n";
        return send_response(client_fd, 404, "text/plain", body, strlen(body));
    }

    /* detect simple content type */
    const char *ct = "application/octet-stream";
    if      (strstr(path, ".html") || strstr(path, ".htm")) ct = "text/html";
    else if (strstr(path, ".css"))   ct = "text/css";
    else if (strstr(path, ".js"))    ct = "application/javascript";
    else if (strstr(path, ".json"))  ct = "application/json";
    else if (strstr(path, ".txt"))   ct = "text/plain";
    else if (strstr(path, ".png"))   ct = "image/png";
    else if (strstr(path, ".jpg") || strstr(path, ".jpeg")) ct = "image/jpeg";
    else if (strstr(path, ".gif"))   ct = "image/gif";

    /* read file into buffer (up to SEND_BUF_SIZE) */
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(ffd);
        const char *body = "500 Internal Server Error\n";
        return send_response(client_fd, 500, "text/plain", body, strlen(body));
    }

    ssize_t r = read(ffd, buf, (size_t)st.st_size);
    close(ffd);

    ssize_t sent = send_response(client_fd, 200, ct, buf, r > 0 ? (size_t)r : 0);
    free(buf);
    return sent;
}

/* Built-in /health endpoint */
static ssize_t handle_health(int client_fd)
{
    const char *body = "{\"status\":\"ok\"}\n";
    return send_response(client_fd, 200, "application/json", body, strlen(body));
}

/* Built-in /echo endpoint – echoes back request URI info */
static ssize_t handle_echo(int client_fd, const HttpRequest *req)
{
    char body[512];
    size_t blen = (size_t)snprintf(body, sizeof body,
        "method: %s\nuri:    %s\nver:    %s\n",
        req->method, req->uri, req->version);
    return send_response(client_fd, 200, "text/plain", body, blen);
}

/*  connection handler*/

typedef struct {
    int  fd;
    char remote_ip[INET6_ADDRSTRLEN];
    int  remote_port;
} Connection;

static void handle_connection(const Connection *conn)
{
    char buf[RECV_BUF_SIZE];
    ssize_t n = recv(conn->fd, buf, sizeof buf - 1, 0);
    if (n <= 0) {
        close(conn->fd);
        return;
    }
    buf[n] = '\0';

    HttpRequest req;
    memset(&req, 0, sizeof req);

    int status      = 200;
    ssize_t sent    = 0;

    if (parse_request_line(buf, &req) < 0) {
        const char *body = "400 Bad Request\n";
        sent   = send_response(conn->fd, 400, "text/plain", body, strlen(body));
        status = 400;
        goto done;
    }

    /* only GET is supported for static files; HEAD is trivial */
    if (strcmp(req.method, "GET") != 0 && strcmp(req.method, "HEAD") != 0) {
        const char *body = "405 Method Not Allowed\n";
        sent   = send_response(conn->fd, 405, "text/plain", body, strlen(body));
        status = 405;
        goto done;
    }

    /* route */
    if (strcmp(req.uri, "/health") == 0) {
        sent = handle_health(conn->fd);
    } else if (strncmp(req.uri, "/echo", 5) == 0) {
        sent = handle_echo(conn->fd, &req);
    } else {
        sent = handle_static_file(conn->fd, req.uri);
        /* infer status from response (simplified) */
        if (sent < 0) status = 500;
    }

done:
    audit_write(conn->remote_ip, conn->remote_port,
                *req.method ? req.method : "-",
                *req.uri    ? req.uri    : "-",
                status, sent);
    close(conn->fd);
}

/* worker thread  */

typedef struct {
    CircularQueue *queue;
    int            worker_id;
} WorkerArg;

static void *worker_thread(void *arg)
{
    WorkerArg     *wa = (WorkerArg *)arg;
    CircularQueue *q  = wa->queue;

    while (1) {
        int fd = queue_pop(q);
        if (fd < 0) break;  /* shutdown signal */

        /* retrieve peer address stored above fd in a tiny sidecar */
        /* We pack peer info in a Connection before pushing; here we
           read it back from the fd's peer.  Use getpeername().      */
        Connection conn;
        conn.fd = fd;

        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
            if (peer.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&peer;
                inet_ntop(AF_INET,  &s->sin_addr,
                          conn.remote_ip, sizeof conn.remote_ip);
                conn.remote_port = ntohs(s->sin_port);
            } else {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&peer;
                inet_ntop(AF_INET6, &s->sin6_addr,
                          conn.remote_ip, sizeof conn.remote_ip);
                conn.remote_port = ntohs(s->sin6_port);
            }
        } else {
            strncpy(conn.remote_ip, "unknown", sizeof conn.remote_ip);
            conn.remote_port = 0;
        }

        handle_connection(&conn);
    }

    return NULL;
}

/*  dispatcher thread */

typedef struct {
    int            listen_fd;
    CircularQueue *queue;
} DispatcherArg;

static void *dispatcher_thread(void *arg)
{
    DispatcherArg *da = (DispatcherArg *)arg;

    while (1) {
        struct sockaddr_storage addr;
        socklen_t addrlen = sizeof addr;

        int client_fd = accept(da->listen_fd,
                               (struct sockaddr *)&addr, &addrlen);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("accept");
            break;
        }

        if (queue_push(da->queue, client_fd) < 0) {
            /* shutting down */
            close(client_fd);
            break;
        }
    }

    return NULL;
}

/*  signal handling */

static volatile sig_atomic_t g_running = 1;

static void sighandler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* main */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <port> <num_workers> [logfile]\n"
            "  port        – listen port (e.g. 8080)\n"
            "  num_workers – worker thread count (1–%d)\n"
            "  logfile     – audit log path (default: audit.log)\n",
            argv[0], MAX_WORKERS);
        return EXIT_FAILURE;
    }

    int port        = atoi(argv[1]);
    int num_workers = atoi(argv[2]);
    const char *logpath = argc >= 4 ? argv[3] : "audit.log";

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (num_workers < 1 || num_workers > MAX_WORKERS) {
        fprintf(stderr, "num_workers must be 1–%d\n", MAX_WORKERS);
        return EXIT_FAILURE;
    }

    /* open audit log */
    g_logfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_logfd < 0) {
        fprintf(stderr, "Warning: cannot open log %s (%s), using stderr\n",
                logpath, strerror(errno));
    }

    /* install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sighandler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* ignore broken-pipe on writes */

    /* create listening socket  */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return EXIT_FAILURE; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); close(listen_fd); return EXIT_FAILURE;
    }
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen"); close(listen_fd); return EXIT_FAILURE;
    }

    fprintf(stdout,
        "httpserver  port=%d  workers=%d  log=%s\n"
        "ctrl c to stop.\n",
        port, num_workers, logpath);

    /*initialise circular queue  */
    CircularQueue queue;
    queue_init(&queue);

    /* spawn worker threads  */
    pthread_t workers[MAX_WORKERS];
    WorkerArg wargs[MAX_WORKERS];
    for (int i = 0; i < num_workers; i++) {
        wargs[i].queue     = &queue;
        wargs[i].worker_id = i;
        if (pthread_create(&workers[i], NULL, worker_thread, &wargs[i]) != 0) {
            perror("pthread_create (worker)");
            return EXIT_FAILURE;
        }
    }

    /* spawn dispatcher thread */
    DispatcherArg darg = { .listen_fd = listen_fd, .queue = &queue };
    pthread_t dispatcher;
    if (pthread_create(&dispatcher, NULL, dispatcher_thread, &darg) != 0) {
        perror("pthread_create (dispatcher)");
        return EXIT_FAILURE;
    }

    /* main thread: wait for shutdown signal */
    while (g_running)
        sleep(1);

    fprintf(stdout, "\nShutting down…\n");

    /* stop accepting; this also unblocks dispatcher's accept() */
    shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);

    pthread_join(dispatcher, NULL);

    /* drain queue and wake workers */
    queue_shutdown(&queue);
    for (int i = 0; i < num_workers; i++)
        pthread_join(workers[i], NULL);

    if (g_logfd >= 0) close(g_logfd);
    fprintf(stdout, "Server stopped cleanly.\n");
    return EXIT_SUCCESS;
}
