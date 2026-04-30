# vibe

A single-header C library for TCP and Unix-socket messaging on Linux.

A background thread runs `epoll`; your thread polls a lock-free queue. Messages are length-prefixed; broadcasts are refcounted instead of copied. ~1600 LOC, no dependencies beyond libc and pthreads. Apache 2.0.

## Contents

- [Quickstart](#quickstart)
- [How it works](#how-it-works)
- [Wire protocol](#wire-protocol)
- [API reference](#api-reference)
- [Examples](#examples)
- [Threading rules](#threading-rules)
- [Configuration](#configuration)
- [Limits and non-goals](#limits-and-non-goals)
- [License](#license)

## Quickstart

```c
#define VIBE_IMPLEMENTATION
#include "vibe.h"
```

Define `VIBE_IMPLEMENTATION` in **exactly one** translation unit, or compile with `-DVIBE_IMPLEMENTATION`. Compile with `-pthread`, GCC or Clang, on Linux.

```sh
make             # builds examples/{chat_server,ping_client,firehose,telemetry_sink}
```

Minimal echo server:

```c
#define VIBE_IMPLEMENTATION
#include "vibe.h"
#include <unistd.h>

int main(void) {
    vibe_hub_t* hub = vibe_hub_start();
    vibe_listen(hub, "0.0.0.0:8080");

    for (;;) {
        vibe_msg_t* batch = vibe_poll(hub);
        if (!batch) { usleep(100); continue; }

        vibe_for_each(msg, batch) {
            if (msg->type == VIBE_EVENT_DATA)
                vibe_send_msg(msg->conn, msg->data, msg->len);
        }
        vibe_free(batch);
    }
}
```

## How it works

- **Hub** owns the epoll fd, an eventfd for cross-thread wakeups, and one IO thread.
- **IO thread** runs `epoll_wait` (edge-triggered, `EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP`). It accepts, reads, frames, and writes. New events go into the **inbox**.
- **Inbox** is an MPSC injector queue with a CAS'd "consuming" bit — multiple application threads MAY call `vibe_poll`; one wins and drains, the others get NULL.
- **Outbox** lives per connection — also an MPSC queue. Any thread can call `vibe_send_msg`. The IO thread is woken via the eventfd and drains writes.
- **Dirty queue** signals which connections have pending writes since the last epoll cycle.
- **Broadcast** allocates the payload once, hands a refcounted `vibe_chunk_t*` to each recipient's outbox, and frees on last release.
- **Backpressure** is per-connection: `pending_bytes` is an atomic counter; once it crosses `VIBE_MAX_PENDING_BYTES` (default 2 GiB), `vibe_send_msg` returns `false`.

There is no internal blocking call you wait on. `vibe_poll` is wait-free. If there's nothing to do, it returns NULL — sleep, spin, or `epoll` your own fds, your call.

## Wire protocol

Every message is framed as:

```
+--------+----------+
|  u32   | payload  |
| length | (N bytes)|
+--------+----------+
```

- 4-byte big-endian length prefix.
- Hard cap on **incoming** frames: 10 MiB. Larger frames cause the connection to be dropped.
- Outgoing frames are limited only by `VIBE_MAX_PENDING_BYTES` and the `uint32_t` length field (4 GiB - 1).
- Receiver delivers whole frames or nothing. Partial frames sit in the per-connection 8 KiB read buffer until complete.

Both peers must speak this framing. If you need HTTP, WebSocket, or anything else, layer it on top.

## API reference

### Types

```c
typedef struct vibe_net_ctx   vibe_hub_t;   // opaque hub
typedef struct vibe_connection vibe_conn_t; // opaque connection (peer or listener)

typedef struct vibe_msg {
    struct vibe_msg* next;  // linked-list of events in this batch
    int              type;  // VIBE_EVENT_DATA | _CONNECTED | _DISCONNECTED
    vibe_conn_t*     conn;  // source/peer connection
    uint32_t         len;   // payload length (0 for non-DATA events)
    char*            data;  // payload pointer (valid until vibe_free(batch))
} vibe_msg_t;

enum {
    VIBE_EVENT_DATA = 0,
    VIBE_EVENT_CONNECTED,
    VIBE_EVENT_DISCONNECTED,
};
```

### Lifecycle

```c
vibe_hub_t* vibe_hub_start(void);
void        vibe_hub_stop(vibe_hub_t* hub);
```

`vibe_hub_start` creates the epoll fd + eventfd and spawns the IO thread. Returns NULL on resource exhaustion.

`vibe_hub_stop` flips the running flag, kicks the eventfd, joins the IO thread, closes both fds, and frees the hub. Drain the inbox first if you care about not leaking the last batch.

### Wiring

```c
bool vibe_listen(vibe_hub_t* hub, const char* addr);
void vibe_dial  (vibe_hub_t* hub, const char* addr);
void vibe_close_conn(vibe_conn_t* conn);
```

**Address grammar**:

| Form | Meaning |
|---|---|
| `"0.0.0.0:8080"` | TCP, bind to all interfaces, port 8080 |
| `"127.0.0.1:8080"` | TCP, bind/connect to specific IP |
| `":8080"` or `"8080"` | TCP, port only (uses `INADDR_ANY` for listen) |
| `"@my_pipe"` | Unix abstract socket (Linux-specific) |
| `"/tmp/my.sock"` | Unix filesystem socket |

The format is detected from the first byte: digit or `:` → TCP, otherwise Unix.

`vibe_listen` returns `false` if `socket`, `bind`, or `listen` fails. Listener backlog is 512. `SO_REUSEADDR` is set. For TCP peers, `TCP_NODELAY` is also set on accepted sockets.

`vibe_dial` is non-blocking. Success/failure of the connect arrives later as a `VIBE_EVENT_CONNECTED` event (failure is currently silent — the socket is closed and no event fires; check with a timeout).

`vibe_close_conn` shuts the socket and releases the connection's reference. The IO thread will deliver a `DISCONNECTED` event shortly after.

### Receiving

```c
vibe_msg_t* vibe_poll(vibe_hub_t* hub);
void        vibe_free(vibe_msg_t* batch);
```

`vibe_poll` returns a linked-list batch of up to **256** events, or NULL if the inbox is empty (or another thread is currently draining). It does not block. Iterate with `vibe_for_each(msg, batch) { ... }`. **`msg->data` and `msg->conn` are valid until you call `vibe_free`**; copy them out if you need to hold on.

`vibe_free` releases the whole batch back to an internal pool and decrements each `conn`'s refcount.

### Sending

```c
bool vibe_send_msg (vibe_conn_t*  conn,  const void* data, uint32_t len);
bool vibe_multicast(vibe_conn_t** conns, int count, const void* data, uint32_t len);
```

Both copy `data` once into a refcounted chunk. The chunk is dropped only after every recipient finishes writing. The data buffer you passed is yours again as soon as the call returns.

`vibe_send_msg` returns `false` if that one connection is over `VIBE_MAX_PENDING_BYTES`. Try again later or hang up.

`vibe_multicast` skips recipients over the limit and queues to the rest. Returns `true` if at least one recipient accepted, `false` if all were full (in which case the chunk is freed). There is no per-recipient status — if you need that, loop over `vibe_send_msg`.

### Iteration macro

```c
#define vibe_for_each(msg, batch) \
    for (vibe_msg_t* msg = (batch); msg != NULL; msg = msg->next)
```

### Low-level aliases

The "ergonomic" names above are inline wrappers around shorter primitives that share the same ABI. Either set works:

| Ergonomic | Primitive |
|---|---|
| `vibe_hub_start` | `vibe_open` |
| `vibe_hub_stop` | `vibe_close` |
| `vibe_listen` | `vibe_bind` |
| `vibe_dial` | `vibe_connect` |
| `vibe_close_conn` | `vibe_disconnect` |
| `vibe_poll` | `vibe_recv` |
| `vibe_free` | `vibe_drop` |
| `vibe_send_msg` | `vibe_send` |
| `vibe_multicast` | `vibe_cast` |
| `VIBE_EVENT_*` | `VIBE_EVT_*` |

## Examples

All examples live in `examples/`. Build with `make`.

### `chat_server` — accept and broadcast

Tracks connected peers, fans out every received message to all of them via refcounted multicast.

```c
vibe_hub_t* hub = vibe_hub_start();
vibe_listen(hub, "0.0.0.0:8080");

vibe_conn_t* users[1024];
int n = 0;

while (running) {
    vibe_msg_t* batch = vibe_poll(hub);
    if (!batch) { usleep(500); continue; }

    vibe_for_each(msg, batch) {
        switch (msg->type) {
        case VIBE_EVENT_CONNECTED:
            if (n < 1024) users[n++] = msg->conn;
            break;
        case VIBE_EVENT_DATA:
            vibe_multicast(users, n, msg->data, msg->len);
            break;
        case VIBE_EVENT_DISCONNECTED:
            // remove from users[]
            break;
        }
    }
    vibe_free(batch);
}
```

Run: `./examples/chat_server 8080`. Connect with any framed-TCP client.

### `ping_client` — request/response RTT

Dials a peer, waits for `CONNECTED`, then sends `"PING"` five times and measures wall-clock RTT to the response. Demonstrates the connect-then-wait handshake pattern.

```c
vibe_dial(hub, "127.0.0.1:8080");

vibe_conn_t* server = NULL;
while (!server) {
    vibe_msg_t* batch = vibe_poll(hub);
    if (batch) {
        vibe_for_each(msg, batch)
            if (msg->type == VIBE_EVENT_CONNECTED) server = msg->conn;
        vibe_free(batch);
    }
}

for (int i = 0; i < 5; i++) {
    uint64_t t0 = nanos();
    vibe_send_msg(server, "PING", 4);

    for (bool got = false; !got; ) {
        vibe_msg_t* batch = vibe_poll(hub);
        if (!batch) continue;
        vibe_for_each(msg, batch)
            if (msg->type == VIBE_EVENT_DATA) got = true;
        vibe_free(batch);
    }
    printf("Seq %d: %.2f us\n", i, (nanos() - t0) / 1000.0);
}
```

Run: `./examples/ping_client 8080` (against a server that echoes).

### `firehose` — backpressure

Floods a peer with 1 KiB messages, backing off whenever `vibe_send_msg` returns `false` instead of growing the outbox unbounded.

```c
char junk[1024];
while (1) {
    if (vibe_send_msg(conn, junk, sizeof junk)) {
        total_sent++;
    } else {
        usleep(100);  // backpressure — let the IO thread drain
    }
    vibe_msg_t* b = vibe_poll(hub);
    if (b) vibe_free(b);  // drain CONNECTED/DISCONNECTED so they don't pile up
}
```

The lesson: ignoring the return value of `vibe_send_msg` is how you OOM. Once `pending_bytes > VIBE_MAX_PENDING_BYTES`, sends drop on the floor — handle the `false` case.

### `telemetry_sink` — many-to-one ingest

Accepts connections, counts messages and bytes, prints rates once a second.

```c
vibe_listen(hub, "0.0.0.0:9090");
uint64_t msgs = 0, bytes = 0;
time_t last = time(NULL);

while (1) {
    vibe_msg_t* batch = vibe_poll(hub);
    if (batch) {
        vibe_for_each(msg, batch)
            if (msg->type == VIBE_EVENT_DATA) { msgs++; bytes += msg->len; }
        vibe_free(batch);
    } else { usleep(10); }

    time_t now = time(NULL);
    if (now > last) {
        printf("%lu msgs/s | %.2f MB/s\n", msgs, bytes / (1024.0*1024));
        msgs = bytes = 0; last = now;
    }
}
```

## Threading rules

| Function | Safe to call concurrently? |
|---|---|
| `vibe_hub_start`, `vibe_hub_stop` | No — call once each per hub. |
| `vibe_listen`, `vibe_dial` | Yes (any thread, any time after `start`). |
| `vibe_send_msg`, `vibe_multicast` | Yes — outboxes are MPSC. |
| `vibe_close_conn` | Yes. Idempotent at the protocol level (subsequent calls are no-ops). |
| `vibe_poll` | Safe from any thread, but **only one thread drains per call** — others see NULL. Pick one consumer in practice. |
| `vibe_free` | Must be paired with the `vibe_poll` that returned the batch. The batch is owned by the caller, not shared. |

After `vibe_free`, do not touch any `msg->data` or `msg->conn` from that batch — `conn` may have been the last reference and freed.

## Configuration

These are `#define`s overridable before including `vibe.h` in the implementation TU:

| Macro | Default | Meaning |
|---|---:|---|
| `VIBE_MAX_PENDING_BYTES` | 2 GiB | Per-connection outbox byte limit. Sends past this return `false`. |
| `VIBE_MAX_EPOLL_EVENTS` | 128 | Batch size for `epoll_wait`. |

Compile-time constants (not user-overridable, change in source if you must):

| Constant | Value | Notes |
|---|---:|---|
| `VIBE_READ_BUFFER_SIZE` | 8192 | Per-connection read buffer. |
| `VIBE_PROTO_HEADER_SIZE` | 4 | Length-prefix size. |
| Incoming frame cap | 10 MiB | Hardcoded in `vibe_handle_read`; oversized frames close the connection. |
| `vibe_recv` batch cap | 256 | Max events returned per `vibe_poll` call. |
| Listen backlog | 512 | `listen(fd, 512)`. |
| `VIBE_BUFFER_CAPACITY` | 256 | Internal queue ring size. |
| `VIBE_CACHE_LINE_ASSUMED` | 128 (x64/arm64), 32 (arm), 64 (else) | Padding to avoid false sharing. |

Sockets are configured with `SO_REUSEADDR`, `O_NONBLOCK`, and (for TCP) `TCP_NODELAY`. Epoll registrations use `EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP` for peers and `EPOLLIN` for listeners.

## Limits and non-goals

- **Linux only.** Uses `epoll`, `eventfd`, `accept4`, and abstract Unix sockets. No macOS, Windows, or BSD.
- **No TLS.** Run behind a terminator (stunnel, nginx, envoy) if you need it.
- **No UDP.** TCP and Unix stream sockets only.
- **No protocol above the framing.** No HTTP, WebSocket, gRPC. Bring your own.
- **Incoming frames > 10 MiB** drop the connection. Increase the constant in source if you need bigger frames; this is a DoS guard.
- **No graceful shutdown drain.** `vibe_hub_stop` does not flush the inbox or pending outboxes — drain first if it matters.
- **`vibe_dial` failures are silent.** A failed connect closes the fd without firing an event; use a timeout.
- **Single-consumer in practice.** `vibe_poll` is multi-thread-safe but serialized; sharding across hubs scales further than fanning out consumers on one hub.

## License

Apache 2.0. Copyright (c) 2026 Praveen Vaddadi <thynktank@gmail.com>. Contributing: see [CONTRIBUTING.md](CONTRIBUTING.md).
