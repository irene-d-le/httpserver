# httpserver — Multi-threaded HTTP Server in C

A concurrent HTTP/1.1 server built with POSIX system calls and pthreads.

---

## Architecture

```
                    ┌─────────────────────────────────┐
  TCP clients ──►   │       Dispatcher Thread          │
  (accept loop)     │  accept() → push fd to queue     │
                    └──────────────┬──────────────────┘
                                   │  fd
                    ┌──────────────▼──────────────────┐
                    │   Thread-safe Circular Queue     │
                    │   capacity: 256 connections      │
                    │   mutex + two condition vars     │
                    └──────┬────────┬──────┬──────────┘
                           │        │      │
                    ┌──────▼──┐ ┌───▼──┐ ┌▼──────┐
                    │Worker 0 │ │  W1  │ │  WN   │  (configurable pool)
                    │recv/parse│ │      │ │       │
                    │route    │ │      │ │       │
                    │respond  │ │      │ │       │
                    └────┬────┘ └──────┘ └───────┘
                         │
                    ┌────▼─────────────────────────────┐
                    │  Audit Log (mutex-serialised)     │
                    │  [timestamp] IP:port "METHOD /uri"│
                    │  status bytes_sent                │
                    └──────────────────────────────────┘
```

### Key components

| Component | File location | Description |
|-----------|--------------|-------------|
| `CircularQueue` | `httpserver.c` | Lock-free ring buffer guarded by mutex + 2 cond-vars. Dispatcher blocks when full; workers block when empty. |
| Dispatcher thread | `dispatcher_thread()` | Single thread that calls `accept()` in a tight loop and pushes fds to the queue. |
| Worker pool | `worker_thread()` | N threads (user-configured). Each pops an fd, handles the full HTTP request/response cycle, then loops. |
| Audit log | `audit_write()` | Mutex-protected `write()` to a log file. Entries are atomic for lines ≤ PIPE_BUF. |
| Request router | `handle_connection()` | Parses request line, dispatches to `/health`, `/echo`, or static file handler. |

---

## Build

```bash
# requires: gcc, make, pthreads 
make
```

Or manually:
```bash
gcc -Wall -Wextra -O2 -pthread -o httpserver httpserver.c
```

---

## Usage

```bash
./httpserver <port> <num_workers> [logfile]
```

| Argument | Description |
|----------|-------------|
| `port` | TCP port to listen on (e.g. `8080`) |
| `num_workers` | Number of worker threads (1–64) |
| `logfile` | Optional audit log path (default: `audit.log`) |

### Examples

```bash
# 4 workers on port 8080, log to audit.log
./httpserver 8080 4

# 8 workers on port 9090, custom log path
./httpserver 9090 8 /var/log/httpserver.log
```

---

## Built-in Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /health` | Returns `{"status":"ok"}` — useful for load-balancer checks |
| `GET /echo` | Returns the method, URI, and HTTP version of the request |
| `GET /<path>` | Serves a file relative to the current working directory |

### Static file serving
The server serves files from the directory it was started in. For example:
```bash
echo "<h1>Hello</h1>" > index.html
./httpserver 8080 4
curl http://localhost:8080/           # serves index.html
curl http://localhost:8080/style.css  # serves style.css
```

---

## Design Notes

### Thread-safe circular queue
- Ring buffer of `QUEUE_CAPACITY` (256) integer fds.
- One `pthread_mutex_t` protects all state.
- `not_full` condition: dispatcher waits here when queue is at capacity.
- `not_empty` condition: workers wait here when queue is empty.
- Shutdown: sets a flag and broadcasts both conditions to wake all threads.

### Graceful shutdown (Ctrl-C / SIGTERM)
1. Signal handler sets `g_running = 0`.
2. Main thread calls `shutdown()` + `close()` on the listen socket → dispatcher's `accept()` returns with an error, dispatcher exits.
3. `queue_shutdown()` broadcasts to all workers → workers drain remaining fds and exit.
4. Main thread `pthread_join()`s all threads before returning.

### Limitations 
- No TLS/HTTPS.
- No HTTP keep-alive (every connection is `Connection: close`).
- Static file reads are fully buffered in memory (not zero-copy sendfile).
- No directory listing.
- No chunked transfer encoding.

---

## Quick Smoke-Test

```bash
make test
```

This builds, starts a server on port 9090, fires two curl requests (`/health` and `/echo`), then stops the server and prints the audit log.
