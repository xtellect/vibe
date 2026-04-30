/*
 * Copyright (c) 2026 Praveen Vaddadi <thynktank@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * vibe.h - Single-header networking/IPC library
 *
 * Usage:
 *   In exactly ONE .c file, before including:
 *     #define VIBE_IMPLEMENTATION
 *     #include "vibe.h"
 *
 *   In all other files that need the API:
 *     #include "vibe.h"
 *
 *   Or compile with -DVIBE_IMPLEMENTATION.
 */

/* _GNU_SOURCE must be defined before any system headers are included */
#ifdef VIBE_IMPLEMENTATION
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#ifndef VIBE_H
#define VIBE_H

#include <stdbool.h>
#include <stdint.h>

/* --- Types --- */

/* A "Hub" manages your event loop and queues. */
typedef struct vibe_net_ctx vibe_hub_t;

/* A "Conn" represents a remote peer. */
typedef struct vibe_connection vibe_conn_t;

/* Events (Data, Connected, Disconnected) */
enum { VIBE_EVT_DATA = 0, VIBE_EVT_CONNECTED, VIBE_EVT_DISCONNECTED };

/* A Message received from the Hub. */
typedef struct vibe_msg {
  struct vibe_msg* next; /* Linked list for batch processing */
  int type;              /* VIBE_EVT_* */
  vibe_conn_t* conn;     /* The source connection */
  uint32_t len;          /* Payload length */
  char* data;            /* Payload pointer */
} vibe_msg_t;

/* --- Core API --- */

/* Lifecycle */
vibe_hub_t* vibe_open(void);
void vibe_close(vibe_hub_t* hub);

/* Wiring */
bool vibe_bind(vibe_hub_t* hub, const char* addr);
void vibe_connect(vibe_hub_t* hub, const char* addr);
void vibe_disconnect(vibe_conn_t* conn);

/* IO */
vibe_msg_t* vibe_recv(vibe_hub_t* hub);
bool vibe_send(vibe_conn_t* conn, const void* data, uint32_t len);
bool vibe_cast(vibe_conn_t** conns, int count, const void* data, uint32_t len);
void vibe_drop(vibe_msg_t* msg);

/* --- Ergonomic API --- */

/* Standardized event names */
enum {
  VIBE_EVENT_DATA = VIBE_EVT_DATA,
  VIBE_EVENT_CONNECTED = VIBE_EVT_CONNECTED,
  VIBE_EVENT_DISCONNECTED = VIBE_EVT_DISCONNECTED
};

/**
 * vibe_for_each(msg, batch)
 *
 * Iterate through the batch of messages returned by vibe_poll().
 *
 * Usage:
 *   vibe_msg_t* batch = vibe_poll(hub);
 *   if (batch) {
 *       vibe_for_each(msg, batch) {
 *           printf("Got data from %p\n", msg->conn);
 *       }
 *       vibe_free(batch);
 *   }
 */
#define vibe_for_each(ITEM, BATCH) \
  for (vibe_msg_t* ITEM = (BATCH); ITEM != NULL; ITEM = ITEM->next)

/**
 * Start the Hub.
 * Spins up the background IO thread and event loop.
 */
static inline vibe_hub_t* vibe_hub_start(void) { return vibe_open(); }

/**
 * Stop the Hub.
 * Joins the IO thread and releases resources.
 */
static inline void vibe_hub_stop(vibe_hub_t* hub) { vibe_close(hub); }

/**
 * Listen for incoming peers (Server mode).
 * Supports "0.0.0.0:8080" (TCP) or "@my_pipe" (Unix Abstract Sockets).
 */
static inline bool vibe_listen(vibe_hub_t* hub, const char* address) {
  return vibe_bind(hub, address);
}

/**
 * Dial a remote peer (Client mode).
 * Non-blocking (returns immediately). Connection status
 * arrives later as a VIBE_EVENT_CONNECTED event.
 */
static inline void vibe_dial(vibe_hub_t* hub, const char* address) {
  vibe_connect(hub, address);
}

/**
 * Check the inbox (Poll).
 * Returns a linked-list batch of events, or NULL if empty.
 * Non-blocking (wait-free consumer).
 */
static inline vibe_msg_t* vibe_poll(vibe_hub_t* hub) { return vibe_recv(hub); }

/**
 * Free a batch of messages.
 * Must be called after processing a batch from vibe_poll().
 */
static inline void vibe_free(vibe_msg_t* batch) { vibe_drop(batch); }

/**
 * Send a message to a specific connection.
 * Thread-safe and non-blocking.
 */
static inline bool vibe_send_msg(vibe_conn_t* conn, const void* data,
                                 uint32_t len) {
  return vibe_send(conn, data, len);
}

/**
 * Broadcast (Multicast) to multiple connections.
 * Uses zero-copy ref-counting internally for maximum efficiency.
 */
static inline bool vibe_multicast(vibe_conn_t** conns, int count,
                                  const void* data, uint32_t len) {
  return vibe_cast(conns, count, data, len);
}

/**
 * Close a specific connection manually.
 */
static inline void vibe_close_conn(vibe_conn_t* conn) { vibe_disconnect(conn); }

#endif /* VIBE_H */

/* ================================================================
 * IMPLEMENTATION
 * ================================================================ */
#ifdef VIBE_IMPLEMENTATION
#ifndef VIBE_IMPLEMENTATION_DONE
#define VIBE_IMPLEMENTATION_DONE

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Platform & Compiler Abstractions (builtin)
 * ---------------------------------------------------------------- */

#ifndef __GNUC__
#error "A GNUC compatible compiler is required"
#endif

#if defined(_WIN32)
#include <Windows.h>
#define VIBE_OS_WINDOWS
#elif defined(__APPLE__)
#define VIBE_OS_DARWIN
#elif defined(__linux__)
#define VIBE_OS_LINUX
#elif defined(__DragonFly__)
#define VIBE_OS_DRAGONFLY
#elif defined(__FreeBSD__)
#define VIBE_OS_FREEBSD
#elif defined(__NetBSD__)
#define VIBE_OS_NETBSD
#elif defined(__OpenBSD__)
#define VIBE_OS_OPENBSD
#else
#error "target OS is not currently supported"
#endif

#if defined(__x86_64__)
#define VIBE_ARCH_X64
#elif defined(__i386__)
#define VIBE_ARCH_X86
#elif defined(__aarch64__)
#define VIBE_ARCH_ARM64
#elif defined(__arm__) || defined(__thumb__)
#define VIBE_ARCH_ARM
#else
#error "target ARCH is not currently supported"
#endif

#define VIBE_CONTAINER_OF(type, field, field_ptr) \
  ((field_ptr) == NULL)                           \
      ? ((type*)NULL)                             \
      : ((type*)(((char*)(field_ptr)) - __builtin_offsetof(type, field)))

#define VIBE_UNUSED(x) ((void)(x))
#define VIBE_LIKELY(x) __builtin_expect(!!(x), 1)
#define VIBE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define VIBE_UNREACHABLE __builtin_unreachable
#define VIBE_STATIC_ASSERT _Static_assert
#define VIBE_FORCE_INLINE inline __attribute__((__always_inline__))
#define VIBE_NOALIAS restrict
#define VIBE_NULLABLE

#define VIBE_ASSERT(cond, msg) assert((cond) && (msg))
#ifdef NDEBUG
#define VIBE_ASSUME(cond) VIBE_ASSERT((cond), "assumption invalidated")
#else
#define VIBE_ASSUME(cond)                           \
  do {                                              \
    if (VIBE_UNLIKELY(!(cond))) VIBE_UNREACHABLE(); \
  } while (0)
#endif

#define VIBE_UPTR(x) ((uintptr_t)(x))
#define VIBE_WRAPPING_ADD(x, y) ((x) + (y))
#define VIBE_WRAPPING_SUB(x, y) ((x) - (y))
#define VIBE_CHECKED_ADD __builtin_add_overflow
#define VIBE_CHECKED_SUB __builtin_sub_overflow

/* ----------------------------------------------------------------
 * PRNG (random)
 * ---------------------------------------------------------------- */

struct vibe_random {
  uint32_t state;
};

static inline void vibe_random_init(struct vibe_random* rng, uint32_t seed) {
  VIBE_ASSERT(seed != 0, "random seed cannot be zero");
  rng->state = seed;
}

static inline uint32_t vibe_random_next(struct vibe_random* rng) {
  VIBE_ASSUME(rng != 0);

  uint32_t xorshift = rng->state;
  xorshift ^= xorshift << 13;
  xorshift ^= xorshift >> 17;
  xorshift ^= xorshift << 5;
  VIBE_ASSUME(xorshift != 0);

  rng->state = xorshift;
  return xorshift;
}

struct vibe_random_sequence {
  uint32_t iter;
  uint32_t range;
  uint32_t index;
};

static inline void vibe_random_sequence_init(
    struct vibe_random_sequence* VIBE_NOALIAS seq,
    struct vibe_random* VIBE_NOALIAS rng, uint32_t range) {
  VIBE_ASSUME(seq != NULL);
  VIBE_ASSUME(rng != NULL);

  VIBE_ASSERT(range > 0, "empty sequence range");
  VIBE_ASSERT(range < (1 << 16), "sequence range too wide");

  seq->iter = range;
  seq->range = range;
  seq->index = (vibe_random_next(rng) >> 16) % range;
}

static inline bool vibe_random_sequence_next(struct vibe_random_sequence* seq) {
  bool valid = VIBE_CHECKED_SUB(seq->iter, 1, &seq->iter);
  if (VIBE_LIKELY(valid)) {
    uint32_t range = seq->range;
    uint32_t co_prime = range - 1;

    uint32_t index = seq->index + co_prime;
    if (index >= range) {
      index -= range;
    }

    VIBE_ASSUME(index < range);
    seq->index = index;
  }

  return valid;
}

/* ----------------------------------------------------------------
 * Atomic Operations
 * ---------------------------------------------------------------- */

typedef atomic_uintptr_t vibe_atomic_uptr;

static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_load_uptr(vibe_atomic_uptr* ptr) {
  return atomic_load_explicit(ptr, memory_order_relaxed);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_load_uptr_acquire(vibe_atomic_uptr* ptr) {
  return atomic_load_explicit(ptr, memory_order_acquire);
}

static VIBE_FORCE_INLINE void vibe_atomic_store_uptr(vibe_atomic_uptr* ptr,
                                                     uintptr_t value) {
  atomic_store_explicit(ptr, value, memory_order_relaxed);
}
static VIBE_FORCE_INLINE void vibe_atomic_store_uptr_release(
    vibe_atomic_uptr* ptr, uintptr_t value) {
  atomic_store_explicit(ptr, value, memory_order_release);
}

static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_fetch_add_uptr_acquire(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_fetch_add_explicit(ptr, value, memory_order_acquire);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_fetch_add_uptr_release(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_fetch_add_explicit(ptr, value, memory_order_release);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_fetch_sub_uptr_release(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_fetch_sub_explicit(ptr, value, memory_order_release);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_fetch_add_uptr_acq_rel(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_fetch_add_explicit(ptr, value, memory_order_acq_rel);
}

static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_swap_uptr_acquire(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_exchange_explicit(ptr, value, memory_order_acquire);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_swap_uptr_release(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_exchange_explicit(ptr, value, memory_order_release);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_swap_uptr_acq_rel(vibe_atomic_uptr* ptr, uintptr_t value) {
  return atomic_exchange_explicit(ptr, value, memory_order_acq_rel);
}

static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_cas_uptr_acquire(vibe_atomic_uptr* VIBE_NOALIAS ptr,
                             uintptr_t* VIBE_NOALIAS cmp, uintptr_t xchg) {
  return atomic_compare_exchange_strong_explicit(
      ptr, cmp, xchg, memory_order_acquire, memory_order_relaxed);
}
static VIBE_FORCE_INLINE uintptr_t
vibe_atomic_cas_uptr_acq_rel(vibe_atomic_uptr* VIBE_NOALIAS ptr,
                             uintptr_t* VIBE_NOALIAS cmp, uintptr_t xchg) {
  return atomic_compare_exchange_strong_explicit(
      ptr, cmp, xchg, memory_order_acq_rel, memory_order_relaxed);
}

#if defined(VIBE_ARCH_X64)

enum { VIBE_CACHE_LINE_ASSUMED = 128 };
#elif defined(VIBE_ARCH_ARM64)

enum { VIBE_CACHE_LINE_ASSUMED = 128 };
#elif defined(VIBE_ARCH_ARM)

enum { VIBE_CACHE_LINE_ASSUMED = 32 };
#else

enum { VIBE_CACHE_LINE_ASSUMED = 64 };
#endif

static inline void vibe_atomic_hint_backoff(struct vibe_random* rng) {
#if defined(VIBE_OS_DARWIN) && defined(VIBE_ARCH_ARM64)

  VIBE_UNUSED(rng);
  __builtin_arm_wfe();
#else

  uint32_t spin_count = ((vibe_random_next(rng) >> 24) & (128 - 1)) | (32 - 1);
  while (VIBE_CHECKED_SUB(spin_count, 1, &spin_count)) {
#if defined(VIBE_OS_WINDOWS)
    YieldProcessor();
#elif defined(VIBE_ARCH_X64) || defined(VIBE_ARCH_X86)
    __builtin_ia32_pause();
#elif defined(VIBE_ARCH_ARM) || defined(VIBE_ARCH_ARM64)
    __builtin_arm_yield()
#else
#error "architecture not supported for emitting CPU yield hint"
#endif
  }
#endif
}

/* ----------------------------------------------------------------
 * Lock-Free Queue
 * ---------------------------------------------------------------- */

/* --- Data Structures --- */

struct vibe_queue_node {
  vibe_atomic_uptr next;
};

struct vibe_queue_list {
  struct vibe_queue_node* head;
  struct vibe_queue_node* tail;
};

struct vibe_queue_injector {
  vibe_atomic_uptr head;
  uint8_t
      _head_cache_padding[VIBE_CACHE_LINE_ASSUMED - sizeof(vibe_atomic_uptr)];

  vibe_atomic_uptr tail;
  uint8_t
      _tail_cache_padding[VIBE_CACHE_LINE_ASSUMED - sizeof(vibe_atomic_uptr)];
};

typedef struct vibe_queue_node* vibe_queue_consumer;

enum {

  VIBE_BUFFER_CAPACITY = 256,
};

struct vibe_queue_buffer {
  vibe_atomic_uptr head;
  uint8_t _cache_padding[VIBE_CACHE_LINE_ASSUMED - sizeof(vibe_atomic_uptr)];

  vibe_atomic_uptr tail;
  vibe_atomic_uptr array[VIBE_BUFFER_CAPACITY];
};

typedef uint64_t vibe_queue_producer;

/* --- Node & List Inline Implementations --- */

static inline void vibe_queue_node_set_next(struct vibe_queue_node* node,
                                            struct vibe_queue_node* next) {
  vibe_atomic_store_uptr(&node->next, VIBE_UPTR(next));
}

static inline struct vibe_queue_node* vibe_queue_node_get_next(
    struct vibe_queue_node* node) {
  return (struct vibe_queue_node*)vibe_atomic_load_uptr(&node->next);
}

static inline void vibe_queue_list_push(
    struct vibe_queue_list* VIBE_NOALIAS list,
    struct vibe_queue_node* VIBE_NOALIAS node) {
  VIBE_ASSUME(list != NULL);
  VIBE_ASSUME(node != NULL);

  struct vibe_queue_node* tail = list->tail;
  list->tail = node;
  vibe_queue_node_set_next(node, NULL);

  if (VIBE_LIKELY(tail != NULL)) {
    vibe_queue_node_set_next(tail, node);
  } else {
    list->head = node;
  }
}

static inline bool vibe_queue_list_push_all(
    struct vibe_queue_list* VIBE_NOALIAS list,
    const struct vibe_queue_list* VIBE_NOALIAS target) {
  VIBE_ASSUME(list != NULL);
  VIBE_ASSUME(target != NULL);

  struct vibe_queue_node* target_head = target->head;
  if (VIBE_UNLIKELY(target_head == NULL)) {
    return false;
  }

  struct vibe_queue_node* target_tail = target->tail;
  VIBE_ASSERT(target_tail != NULL, "invalid target list with head but no tail");
  VIBE_ASSERT(vibe_queue_node_get_next(target_tail) == NULL,
              "invalid target list tail but next != NULL");

  struct vibe_queue_node* head = list->head;
  if (VIBE_UNLIKELY(head == NULL)) {
    list->head = target_head;
    list->tail = target_tail;
    return true;
  }

  struct vibe_queue_node* tail = list->tail;
  VIBE_ASSERT(tail != NULL, "invalid list with head but no tail");
  VIBE_ASSERT(vibe_queue_node_get_next(tail) == NULL,
              "invalid list with tail but next != NULL");

  vibe_queue_node_set_next(tail, target_head);
  list->tail = target_tail;
  return true;
}

static inline struct vibe_queue_node* vibe_queue_list_pop(
    struct vibe_queue_list* list) {
  VIBE_ASSUME(list != NULL);

  struct vibe_queue_node* node = list->head;
  if (VIBE_LIKELY(node != NULL)) {
    struct vibe_queue_node* next = vibe_queue_node_get_next(node);
    list->head = next;

    if (next == NULL) {
      VIBE_ASSERT(list->tail != NULL, "invalid list with head but no tail");
      list->tail = NULL;
    }
  }

  return node;
}

/* --- Injector Constants --- */

static const uintptr_t VIBE_INJECT_CONSUMING_BIT = VIBE_UPTR(1);
static const uintptr_t VIBE_INJECT_NODE_MASK = ~VIBE_UPTR(1);

/* --- Injector Implementations --- */

static inline void vibe_queue_injector_push(
    struct vibe_queue_injector* VIBE_NOALIAS injector,
    struct vibe_queue_list* VIBE_NOALIAS list) {
  VIBE_ASSUME(injector != NULL);
  VIBE_ASSUME(list != NULL);

  struct vibe_queue_node* front = list->head;
  if (front == NULL) {
    return;
  }

  struct vibe_queue_node* back = list->tail;
  VIBE_ASSERT(back != NULL, "pushing invalid list with head but no tail");
  VIBE_ASSERT(vibe_queue_node_get_next(back) == NULL,
              "pushing invalid list with tail->next != NULL");

  uintptr_t tail =
      vibe_atomic_swap_uptr_release(&injector->tail, VIBE_UPTR(back));

  struct vibe_queue_node* prev = (struct vibe_queue_node*)tail;
  if (VIBE_LIKELY(prev != NULL)) {
    vibe_atomic_store_uptr_release(&prev->next, VIBE_UPTR(front));
    return;
  }

  VIBE_ASSUME((VIBE_UPTR(front) & VIBE_INJECT_CONSUMING_BIT) == 0);
  uintptr_t head =
      vibe_atomic_fetch_add_uptr_release(&injector->head, VIBE_UPTR(front));
  VIBE_ASSERT((head & VIBE_INJECT_NODE_MASK) == 0,
              "injector pushed to head when it was not empty");
}

static inline vibe_queue_consumer vibe_queue_consumer_acquire(
    struct vibe_queue_injector* VIBE_NOALIAS injector) {
  VIBE_ASSUME(injector != NULL);

  uintptr_t head = vibe_atomic_load_uptr(&injector->head);
  while (true) {
    struct vibe_queue_node* head_node =
        (struct vibe_queue_node*)(head & VIBE_INJECT_NODE_MASK);
    if (VIBE_LIKELY(head_node == NULL)) {
      return NULL;
    }

    if (VIBE_UNLIKELY((head & VIBE_INJECT_CONSUMING_BIT) != 0)) {
      return NULL;
    }

    if (VIBE_LIKELY(vibe_atomic_cas_uptr_acquire(
            &injector->head, &head, VIBE_UPTR(VIBE_INJECT_CONSUMING_BIT)))) {
      return head_node;
    }
  }
}

static inline struct vibe_queue_node* vibe_queue_consumer_pop(
    vibe_queue_consumer* VIBE_NOALIAS consumer,
    struct vibe_queue_injector* VIBE_NOALIAS injector) {
  VIBE_ASSUME(consumer != NULL);
  VIBE_ASSUME(injector != NULL);

  struct vibe_queue_node* node = *consumer;
  if (VIBE_UNLIKELY(node == NULL)) {
    uintptr_t head = vibe_atomic_load_uptr_acquire(&injector->head);
    VIBE_ASSERT((head & VIBE_INJECT_CONSUMING_BIT) != 0,
                "consumer popped from injector without the CONSUMING bit");

    node = (struct vibe_queue_node*)(head & VIBE_INJECT_NODE_MASK);
    if (VIBE_UNLIKELY(node == NULL)) {
      return NULL;
    }

    *consumer = node;
    vibe_atomic_store_uptr(&injector->head,
                           VIBE_UPTR(VIBE_INJECT_CONSUMING_BIT));
  }

  struct vibe_queue_node* next =
      (struct vibe_queue_node*)vibe_atomic_load_uptr_acquire(&node->next);
  if (VIBE_UNLIKELY(next == NULL)) {
    uintptr_t current_tail = VIBE_UPTR(node);
    if (VIBE_UNLIKELY(!vibe_atomic_cas_uptr_acq_rel(
            &injector->tail, &current_tail, VIBE_UPTR(0)))) {
      do {
#if defined(VIBE_ARCH_X64) || defined(VIBE_ARCH_X86)
        __builtin_ia32_pause();
#elif defined(VIBE_ARCH_ARM) || defined(VIBE_ARCH_ARM64)
        __builtin_arm_yield();
#endif
        next =
            (struct vibe_queue_node*)vibe_atomic_load_uptr_acquire(&node->next);
      } while (next == NULL);
    }
  }

  *consumer = next;
  return node;
}

static inline void vibe_queue_consumer_release(
    vibe_queue_consumer VIBE_NOALIAS consumer,
    struct vibe_queue_injector* VIBE_NOALIAS injector) {
  VIBE_ASSUME(injector != NULL);

  if (VIBE_UNLIKELY(consumer != NULL)) {
    vibe_atomic_store_uptr_release(&injector->head, VIBE_UPTR(consumer));
    return;
  }

  uintptr_t head = vibe_atomic_fetch_sub_uptr_release(
      &injector->head, VIBE_INJECT_CONSUMING_BIT);
  VIBE_ASSERT((head & VIBE_INJECT_CONSUMING_BIT) != 0,
              "consumer was popping without having set the CONSUMING bit");
}

/* --- Producer Prototypes & Helpers --- */

enum {
  VIBE_PROD_HEAD_SHIFT = 0,
  VIBE_PROD_TAIL_SHIFT = 16,
  VIBE_PROD_PUSHED_SHIFT = 32,
  VIBE_PROD_AVAILABLE_SHIFT = 48,
};

static inline vibe_queue_producer vibe_queue_producer_init(
    struct vibe_queue_buffer* VIBE_NOALIAS buffer);
static inline bool vibe_queue_producer_push(
    vibe_queue_producer* VIBE_NOALIAS producer,
    struct vibe_queue_buffer* VIBE_NOALIAS buffer,
    struct vibe_queue_node* VIBE_NOALIAS node);
static inline struct vibe_queue_node* vibe_queue_producer_pop(
    vibe_queue_producer* VIBE_NOALIAS producer,
    struct vibe_queue_buffer* VIBE_NOALIAS buffer);
static inline bool vibe_queue_producer_commit(
    vibe_queue_producer producer,
    struct vibe_queue_buffer* VIBE_NOALIAS buffer);

/* --- Buffer Implementations --- */

static inline void vibe_queue_buffer_push(
    struct vibe_queue_buffer* VIBE_NOALIAS buffer,
    struct vibe_queue_node* VIBE_NOALIAS node,
    struct vibe_queue_list* VIBE_NOALIAS overflowed) {
  VIBE_ASSUME(buffer != NULL);
  VIBE_ASSUME(node != NULL);
  VIBE_ASSUME(overflowed != NULL);

  uint16_t head = (uint16_t)vibe_atomic_load_uptr(&buffer->head);
  uint16_t tail = (uint16_t)vibe_atomic_load_uptr(&buffer->tail);

  uint16_t size = VIBE_WRAPPING_SUB(tail, head);
  VIBE_ASSERT(size <= VIBE_BUFFER_CAPACITY, "invalid buffer size when pushing");

  if (VIBE_UNLIKELY(size == VIBE_BUFFER_CAPACITY)) {
    uint16_t new_head = VIBE_WRAPPING_ADD(head, size / 2);

    uintptr_t current_head = VIBE_UPTR(head);
    if (VIBE_LIKELY(vibe_atomic_cas_uptr_acquire(&buffer->head, &current_head,
                                                 VIBE_UPTR(new_head)))) {
      while (VIBE_LIKELY(head != new_head)) {
        uintptr_t stolen =
            vibe_atomic_load_uptr(&buffer->array[head % VIBE_BUFFER_CAPACITY]);
        head = VIBE_WRAPPING_ADD(head, 1);

        struct vibe_queue_node* migrated = (struct vibe_queue_node*)stolen;
        VIBE_ASSERT(migrated != NULL, "buffer_push() migrated an invalid node");
        vibe_queue_list_push(overflowed, migrated);
      }
      vibe_queue_list_push(overflowed, node);
      return;
    }

    size = VIBE_WRAPPING_SUB(tail, (uint16_t)current_head);
  }

  VIBE_ASSERT(size < VIBE_BUFFER_CAPACITY,
              "invalid buffer size when trying to push to array");
  vibe_atomic_store_uptr(&buffer->array[tail % VIBE_BUFFER_CAPACITY],
                         VIBE_UPTR(node));
  vibe_atomic_store_uptr_release(&buffer->tail, VIBE_WRAPPING_ADD(tail, 1));
}

static inline uintptr_t vibe_queue_buffer_pop(
    struct vibe_queue_buffer* buffer) {
  VIBE_ASSUME(buffer != NULL);

  uint16_t head =
      (uint16_t)vibe_atomic_fetch_add_uptr_acquire(&buffer->head, 1);
  uint16_t tail = (uint16_t)vibe_atomic_load_uptr(&buffer->tail);

  uint16_t size = VIBE_WRAPPING_SUB(tail, head);
  VIBE_ASSERT(size <= VIBE_BUFFER_CAPACITY, "invalid buffer size when popping");

  if (VIBE_UNLIKELY(size == 0)) {
    vibe_atomic_store_uptr(&buffer->head, VIBE_UPTR(head));
    return 0;
  }

  uintptr_t stolen =
      vibe_atomic_load_uptr(&buffer->array[head % VIBE_BUFFER_CAPACITY]);
  struct vibe_queue_node* node = (struct vibe_queue_node*)stolen;
  VIBE_ASSERT(node != NULL, "returning an invalid stolen node");
  return VIBE_UPTR(node);
}

/* Internal helper for steal/inject/fill */
static inline uintptr_t vibe_queue_buffer_report_stolen(
    struct vibe_queue_buffer* buffer,
    vibe_queue_producer* VIBE_NOALIAS producer) {
  VIBE_ASSUME(buffer != NULL);
  VIBE_ASSUME(producer != NULL);

  struct vibe_queue_node* node = vibe_queue_producer_pop(producer, buffer);
  if (VIBE_UNLIKELY(node == NULL)) {
    return 0;
  }

  bool pushed = vibe_queue_producer_commit(*producer, buffer);
  VIBE_ASSERT((VIBE_UPTR(node) & 1) == 0, "returning a misaligned stolen node");
  return VIBE_UPTR(node) | VIBE_UPTR(pushed);
}

static inline uintptr_t vibe_queue_buffer_steal(
    struct vibe_queue_buffer* VIBE_NOALIAS buffer,
    struct vibe_queue_buffer* VIBE_NOALIAS target,
    struct vibe_random* VIBE_NOALIAS rng) {
  VIBE_ASSUME(buffer != NULL);
  VIBE_ASSUME(target != NULL);
  VIBE_ASSUME(buffer != target);
  VIBE_ASSUME(rng != NULL);

  while (true) {
    uint16_t target_head =
        (uint16_t)vibe_atomic_load_uptr_acquire(&target->head);
    uint16_t target_tail =
        (uint16_t)vibe_atomic_load_uptr_acquire(&target->tail);

    uint16_t target_size = VIBE_WRAPPING_SUB(target_tail, target_head);
    if (VIBE_UNLIKELY(((int16_t)target_size) <= 0)) {
      return 0;
    }

    uint16_t target_steal = target_size - (target_size / 2);
    if (VIBE_UNLIKELY(target_steal > (VIBE_BUFFER_CAPACITY / 2))) {
      continue;
    }

    uint16_t new_target_head = target_head;
    vibe_queue_producer producer = vibe_queue_producer_init(buffer);

    while (VIBE_LIKELY(VIBE_CHECKED_SUB(target_steal, 1, &target_steal))) {
      uintptr_t stolen = vibe_atomic_load_uptr(
          &target->array[new_target_head % VIBE_BUFFER_CAPACITY]);
      new_target_head = VIBE_WRAPPING_ADD(new_target_head, 1);

      struct vibe_queue_node* node = (struct vibe_queue_node*)stolen;
      VIBE_ASSERT(node != NULL,
                  "buffer is stealing an invalid node from target");

      bool pushed = vibe_queue_producer_push(&producer, buffer, node);
      VIBE_ASSERT(pushed, "buffer is stealing when not empty");
    }

    uintptr_t current_target_head = VIBE_UPTR(target_head);
    if (VIBE_UNLIKELY(!vibe_atomic_cas_uptr_acq_rel(
            &target->head, &current_target_head, VIBE_UPTR(new_target_head)))) {
      vibe_atomic_hint_backoff(rng);
      continue;
    }

    return vibe_queue_buffer_report_stolen(buffer, &producer);
  }
}

static inline uintptr_t vibe_queue_buffer_inject(
    struct vibe_queue_buffer* VIBE_NOALIAS buffer,
    struct vibe_queue_injector* VIBE_NOALIAS injector) {
  VIBE_ASSUME(buffer != NULL);
  VIBE_ASSUME(injector != NULL);

  vibe_queue_consumer consumer = vibe_queue_consumer_acquire(injector);
  if (VIBE_LIKELY(consumer == NULL)) {
    return 0;
  }

  vibe_queue_producer producer = vibe_queue_producer_init(buffer);
  uint16_t available = VIBE_BUFFER_CAPACITY;

  while (VIBE_LIKELY(VIBE_CHECKED_SUB(available, 1, &available))) {
    struct vibe_queue_node* node = vibe_queue_consumer_pop(&consumer, injector);
    if (VIBE_UNLIKELY(node == NULL)) {
      break;
    }

    bool pushed = vibe_queue_producer_push(&producer, buffer, node);
    VIBE_ASSERT(pushed, "buffer is injecting when not empty");
  }

  vibe_queue_consumer_release(consumer, injector);
  return vibe_queue_buffer_report_stolen(buffer, &producer);
}

static inline uintptr_t vibe_queue_buffer_fill(
    struct vibe_queue_buffer* VIBE_NOALIAS buffer,
    struct vibe_queue_list* VIBE_NOALIAS list) {
  VIBE_ASSUME(buffer != NULL);
  VIBE_ASSUME(list != NULL);

  vibe_queue_producer producer = vibe_queue_producer_init(buffer);
  uint16_t available = VIBE_BUFFER_CAPACITY;

  while (VIBE_LIKELY(VIBE_CHECKED_SUB(available, 1, &available))) {
    struct vibe_queue_node* node = vibe_queue_list_pop(list);
    if (VIBE_UNLIKELY(node == NULL)) {
      break;
    }

    bool pushed = vibe_queue_producer_push(&producer, buffer, node);
    VIBE_ASSERT(pushed, "buffer is filling when not empty");
  }

  return vibe_queue_buffer_report_stolen(buffer, &producer);
}

/* --- Producer Implementations --- */

static inline vibe_queue_producer vibe_queue_producer_init(
    struct vibe_queue_buffer* VIBE_NOALIAS buffer) {
  uint16_t head = (uint16_t)vibe_atomic_load_uptr(&buffer->head);
  uint16_t tail = (uint16_t)(*((uintptr_t*)&buffer->tail));

  uint16_t size = VIBE_WRAPPING_SUB(tail, head);
  VIBE_ASSERT(size <= VIBE_BUFFER_CAPACITY,
              "invalid buffer size when producing");

  uint64_t position = 0;
  position |= ((uint64_t)head) << VIBE_PROD_HEAD_SHIFT;
  position |= ((uint64_t)tail) << VIBE_PROD_TAIL_SHIFT;
  position |= ((uint64_t)0) << VIBE_PROD_PUSHED_SHIFT;
  position |= ((uint64_t)(VIBE_BUFFER_CAPACITY - size))
              << VIBE_PROD_AVAILABLE_SHIFT;
  return position;
}

static inline bool vibe_queue_producer_push(
    vibe_queue_producer* VIBE_NOALIAS producer,
    struct vibe_queue_buffer* VIBE_NOALIAS buffer,
    struct vibe_queue_node* VIBE_NOALIAS node) {
  VIBE_ASSUME(producer != NULL);
  VIBE_ASSUME(buffer != NULL);
  VIBE_ASSUME(node != NULL);

  uint64_t position = *producer;
  uint16_t tail = (uint16_t)(position >> VIBE_PROD_TAIL_SHIFT);
  uint16_t pushed = (uint16_t)(position >> VIBE_PROD_PUSHED_SHIFT);
  uint16_t available = (uint16_t)(position >> VIBE_PROD_AVAILABLE_SHIFT);

  if (VIBE_UNLIKELY(available == 0)) {
    return false;
  }

  position = VIBE_WRAPPING_ADD(position, 1ULL << VIBE_PROD_PUSHED_SHIFT);
  position = VIBE_WRAPPING_SUB(position, 1ULL << VIBE_PROD_AVAILABLE_SHIFT);
  *producer = position;

  vibe_atomic_uptr* slot =
      &buffer->array[VIBE_WRAPPING_ADD(tail, pushed) % VIBE_BUFFER_CAPACITY];
  vibe_atomic_store_uptr(slot, VIBE_UPTR(node));
  return true;
}

static inline struct vibe_queue_node* vibe_queue_producer_pop(
    vibe_queue_producer* VIBE_NOALIAS producer,
    struct vibe_queue_buffer* VIBE_NOALIAS buffer) {
  VIBE_ASSUME(producer != NULL);
  VIBE_ASSUME(buffer != NULL);

  uint64_t position = *producer;
  uint16_t tail = (uint16_t)(position >> VIBE_PROD_TAIL_SHIFT);
  uint16_t pushed = (uint16_t)(position >> VIBE_PROD_PUSHED_SHIFT);

  if (VIBE_UNLIKELY(pushed == 0)) {
    return NULL;
  }

  position = VIBE_WRAPPING_SUB(position, 1ULL << VIBE_PROD_PUSHED_SHIFT);
  position = VIBE_WRAPPING_ADD(position, 1ULL << VIBE_PROD_AVAILABLE_SHIFT);
  *producer = position;

  vibe_atomic_uptr* slot =
      &buffer->array[VIBE_WRAPPING_ADD(tail, pushed) % VIBE_BUFFER_CAPACITY];
  struct vibe_queue_node* node =
      (struct vibe_queue_node*)vibe_atomic_load_uptr(slot);
  VIBE_ASSERT(node != NULL, "producer popped a node that was never pushed");
  return node;
}

static inline bool vibe_queue_producer_commit(
    vibe_queue_producer producer,
    struct vibe_queue_buffer* VIBE_NOALIAS buffer) {
  VIBE_ASSUME(producer != 0);
  VIBE_ASSUME(buffer != NULL);

  uint64_t position = producer;
  uint16_t tail = (uint16_t)(position >> VIBE_PROD_TAIL_SHIFT);
  uint16_t pushed = (uint16_t)(position >> VIBE_PROD_PUSHED_SHIFT);

  bool committed = pushed > 0;
  if (VIBE_LIKELY(committed)) {
    uint16_t new_tail = VIBE_WRAPPING_ADD(tail, pushed);
    vibe_atomic_store_uptr_release(&buffer->tail, new_tail);
  }

  return committed;
}

/* ----------------------------------------------------------------
 * Network Engine Implementation
 * ---------------------------------------------------------------- */

enum {
  VIBE_READ_BUFFER_SIZE = 8192,
  VIBE_PROTO_HEADER_SIZE = 4,
  VIBE_MAX_MSG_SIZE = 2U * 1024 * 1024 * 1024 /* 2GB */
};

#ifndef VIBE_MAX_EPOLL_EVENTS
#define VIBE_MAX_EPOLL_EVENTS 128
#endif

#ifndef VIBE_MAX_PENDING_BYTES
#define VIBE_MAX_PENDING_BYTES (2U * 1024 * 1024 * 1024)
#endif

static int vibe_g_notify_sentinel = -1;

typedef struct vibe_chunk {
  vibe_atomic_uptr ref_count;
  uint32_t len;
  char data[];
} vibe_chunk_t;

typedef struct vibe_node {
  struct vibe_queue_node qnode;
  struct vibe_node* next_msg;
  int type;
  vibe_conn_t* conn;
  uint32_t len;
  char* data_ptr;
  union {
    struct {
      vibe_chunk_t* chunk;
      uint32_t header_off;
      uint32_t payload_off;
      uint8_t header_buf[4];
    } out;
  };
  char inline_data[];
} vibe_node_t;

struct vibe_connection {
  struct vibe_queue_node dirty_node;
  int fd;
  atomic_int ref_count;
  atomic_int is_dirty;
  bool active;
  bool is_listener;
  struct vibe_queue_injector outbox;
  atomic_size_t pending_bytes;
  vibe_node_t* partial_msg;
  uint32_t read_idx;
  uint8_t read_buf[VIBE_READ_BUFFER_SIZE];
  vibe_hub_t* ctx;
};

struct vibe_net_ctx {
  int epoll_fd;
  int notify_fd;
  volatile bool running;
  pthread_t io_thread;
  struct vibe_queue_injector inbox;
  struct vibe_queue_injector dirty_queue;
};

static struct vibe_queue_injector vibe_g_node_pool;

static void vibe_conn_retain(vibe_conn_t* conn) {
  atomic_fetch_add_explicit(&conn->ref_count, 1, memory_order_relaxed);
}

static void vibe_conn_release(vibe_conn_t* conn) {
  if (atomic_fetch_sub_explicit(&conn->ref_count, 1, memory_order_acq_rel) ==
      1) {
    atomic_thread_fence(memory_order_acquire);
    if (conn->fd >= 0) close(conn->fd);
    free(conn);
  }
}

static void vibe_chunk_release(vibe_chunk_t* chunk) {
  if (atomic_fetch_sub_explicit(&chunk->ref_count, 1, memory_order_acq_rel) ==
      1) {
    atomic_thread_fence(memory_order_acquire);
    free(chunk);
  }
}

static void vibe_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void vibe_tune_socket(int fd, int family) {
  vibe_set_nonblocking(fd);
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (family == AF_INET) {
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  }
}

static int vibe_parse_addr(const char* str, struct sockaddr_un* un,
                           struct sockaddr_in* in) {
  if (str[0] == ':' || (str[0] >= '0' && str[0] <= '9')) {
    in->sin_family = AF_INET;
    const char* colon = strchr(str, ':');
    if (colon) {
      char ip[64] = {0};
      if (colon > str)
        strncpy(ip, str, colon - str);
      else
        strcpy(ip, "0.0.0.0");
      inet_pton(AF_INET, ip, &in->sin_addr);
      in->sin_port = htons(atoi(colon + 1));
    } else {
      in->sin_addr.s_addr = INADDR_ANY;
      in->sin_port = htons(atoi(str));
    }
    return AF_INET;
  } else {
    un->sun_family = AF_UNIX;
    strncpy(un->sun_path, str, sizeof(un->sun_path) - 1);
    if (str[0] == '@') un->sun_path[0] = '\0';
    return AF_UNIX;
  }
}

static vibe_node_t* vibe_alloc_node(size_t extra_size) {
  if (extra_size == 0) {
    vibe_queue_consumer c = vibe_queue_consumer_acquire(&vibe_g_node_pool);
    if (c) {
      struct vibe_queue_node* n =
          vibe_queue_consumer_pop(&c, &vibe_g_node_pool);
      vibe_queue_consumer_release(c, &vibe_g_node_pool);
      if (n) {
        vibe_node_t* node = VIBE_CONTAINER_OF(vibe_node_t, qnode, n);
        memset(node, 0, sizeof(vibe_node_t));
        return node;
      }
    }
  }
  return calloc(1, offsetof(vibe_node_t, inline_data) + extra_size);
}

static void vibe_free_node(vibe_node_t* node) {
  if (node->out.chunk != NULL) {
    struct vibe_queue_list l = {.head = &node->qnode, .tail = &node->qnode};
    vibe_queue_node_set_next(&node->qnode, NULL);
    vibe_queue_injector_push(&vibe_g_node_pool, &l);
  } else {
    free(node);
  }
}

static void vibe_schedule_dirty(vibe_conn_t* conn) {
  int expected = 0;
  if (atomic_compare_exchange_strong(&conn->is_dirty, &expected, 1)) {
    vibe_conn_retain(conn);
    struct vibe_queue_list l = {.head = &conn->dirty_node,
                                .tail = &conn->dirty_node};
    vibe_queue_node_set_next(&conn->dirty_node, NULL);

    vibe_queue_injector_push(&conn->ctx->dirty_queue, &l);
    uint64_t one = 1;
    if (write(conn->ctx->notify_fd, &one, sizeof(one)) < 0) {
    }
  }
}

static void vibe_handle_write(vibe_conn_t* conn) {
  do {
    while (true) {
      vibe_node_t* msg = conn->partial_msg;
      if (!msg) {
        vibe_queue_consumer c = vibe_queue_consumer_acquire(&conn->outbox);
        if (!c) break;
        struct vibe_queue_node* qn = vibe_queue_consumer_pop(&c, &conn->outbox);
        vibe_queue_consumer_release(c, &conn->outbox);
        if (!qn) break;
        msg = VIBE_CONTAINER_OF(vibe_node_t, qnode, qn);
        conn->partial_msg = msg;
        uint32_t net_len = htonl(msg->out.chunk->len);
        memcpy(msg->out.header_buf, &net_len, 4);
      }

      struct iovec iov[2];
      int iov_cnt = 0;
      if (msg->out.header_off < 4) {
        iov[iov_cnt].iov_base = msg->out.header_buf + msg->out.header_off;
        iov[iov_cnt].iov_len = 4 - msg->out.header_off;
        iov_cnt++;
      }
      iov[iov_cnt].iov_base = msg->out.chunk->data + msg->out.payload_off;
      iov[iov_cnt].iov_len = msg->out.chunk->len - msg->out.payload_off;
      iov_cnt++;

      struct msghdr mh = {0};
      mh.msg_iov = iov;
      mh.msg_iovlen = iov_cnt;

      ssize_t n = sendmsg(conn->fd, &mh, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        conn->active = false;
        return;
      }

      size_t written = (size_t)n;
      atomic_fetch_sub_explicit(&conn->pending_bytes, written,
                                memory_order_relaxed);
      if (msg->out.header_off < 4) {
        size_t h_needed = 4 - msg->out.header_off;
        size_t h_wrote = (written < h_needed) ? written : h_needed;
        msg->out.header_off += h_wrote;
        written -= h_wrote;
      }
      msg->out.payload_off += written;

      if (msg->out.payload_off == msg->out.chunk->len) {
        vibe_chunk_release(msg->out.chunk);
        vibe_free_node(msg);
        conn->partial_msg = NULL;
      } else {
        return;
      }
    }

    vibe_queue_consumer c = vibe_queue_consumer_acquire(&conn->outbox);
    if (c) {
      vibe_queue_consumer_release(c, &conn->outbox);
      continue;
    }

    int expected = 1;
    if (atomic_compare_exchange_strong(&conn->is_dirty, &expected, 0)) {
      break;
    }
    if (expected == 0) break;
  } while (true);
}

static void vibe_handle_read(vibe_conn_t* conn, struct vibe_queue_list* batch) {
  while (true) {
    ssize_t r = recv(conn->fd, conn->read_buf + conn->read_idx,
                     VIBE_READ_BUFFER_SIZE - conn->read_idx, 0);

    if (r == 0) {
      conn->active = false;
      return;
    }
    if (r < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) conn->active = false;
      return;
    }

    conn->read_idx += r;
    bool grabbed_data = false;

    while (conn->read_idx >= 4) {
      uint32_t net_len;
      memcpy(&net_len, conn->read_buf, 4);
      uint32_t len = ntohl(net_len);

      if (len > 10 * 1024 * 1024) {
        conn->active = false;
        return;
      }
      if (conn->read_idx < 4 + len) break;

      vibe_node_t* node = vibe_alloc_node(len);
      if (!node) {
        conn->active = false;
        return;
      }

      node->type = VIBE_EVT_DATA;
      node->conn = conn;
      node->len = len;
      node->data_ptr = node->inline_data;
      memcpy(node->data_ptr, conn->read_buf + 4, len);

      vibe_conn_retain(conn);
      vibe_queue_list_push(batch, &node->qnode);
      grabbed_data = true;

      size_t frame_size = 4 + len;
      memmove(conn->read_buf, conn->read_buf + frame_size,
              conn->read_idx - frame_size);
      conn->read_idx -= frame_size;
    }

    if (grabbed_data) {
      vibe_schedule_dirty(conn);
    }
  }
}

static void* vibe_io_loop(void* arg) {
  vibe_hub_t* ctx = (vibe_hub_t*)arg;
  struct epoll_event events[VIBE_MAX_EPOLL_EVENTS];

  while (ctx->running) {
    int n;
    do {
      n = epoll_wait(ctx->epoll_fd, events, VIBE_MAX_EPOLL_EVENTS, -1);
    } while (n < 0 && errno == EINTR);

    if (n < 0) break;

    struct vibe_queue_list batch = {0};

    for (int i = 0; i < n; i++) {
      if (events[i].data.ptr == &vibe_g_notify_sentinel) {
        uint64_t val;
        ssize_t rr;
        do {
          rr = read(ctx->notify_fd, &val, sizeof(val));
        } while (rr > 0);
        continue;
      }

      vibe_conn_t* conn = events[i].data.ptr;
      uint32_t ev = events[i].events;

      if (conn->is_listener) {
        while (true) {
          int cfd = accept4(conn->fd, NULL, NULL, SOCK_NONBLOCK);
          if (cfd < 0) break;

          struct sockaddr_storage ss;
          socklen_t sl = sizeof(ss);
          getsockname(cfd, (struct sockaddr*)&ss, &sl);
          vibe_tune_socket(cfd, ss.ss_family);

          vibe_conn_t* peer = calloc(1, sizeof(vibe_conn_t));
          peer->fd = cfd;
          peer->active = true;
          peer->ctx = ctx;
          atomic_store(&peer->ref_count, 1);

          struct epoll_event e = {
              .events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
              .data.ptr = peer};
          if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, cfd, &e) < 0) {
            close(cfd);
            free(peer);
            continue;
          }

          vibe_node_t* en = vibe_alloc_node(0);
          en->type = VIBE_EVT_CONNECTED;
          en->conn = peer;
          vibe_conn_retain(peer);
          vibe_queue_list_push(&batch, &en->qnode);
        }
      } else {
        if (ev & EPOLLIN) vibe_handle_read(conn, &batch);
        if (ev & EPOLLOUT) {
          if (!conn->active) {
            int err = 0;
            socklen_t l = sizeof(err);
            getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &l);
            if (err == 0) {
              conn->active = true;
              vibe_node_t* en = vibe_alloc_node(0);
              en->type = VIBE_EVT_CONNECTED;
              en->conn = conn;
              vibe_conn_retain(conn);
              vibe_queue_list_push(&batch, &en->qnode);
            } else {
              conn->active = false;
            }
          }
          if (conn->active) vibe_handle_write(conn);
        }
        if ((ev & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) || !conn->active) {
          conn->active = false;
          epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
          vibe_node_t* en = vibe_alloc_node(0);
          en->type = VIBE_EVT_DISCONNECTED;
          en->conn = conn;
          vibe_conn_retain(conn);
          vibe_queue_list_push(&batch, &en->qnode);
          vibe_conn_release(conn);
        }
      }
    }

    vibe_queue_consumer dc = vibe_queue_consumer_acquire(&ctx->dirty_queue);
    if (dc) {
      struct vibe_queue_node* dn;
      while ((dn = vibe_queue_consumer_pop(&dc, &ctx->dirty_queue))) {
        vibe_conn_t* dconn = VIBE_CONTAINER_OF(vibe_conn_t, dirty_node, dn);
        if (dconn->active) vibe_handle_write(dconn);
        vibe_conn_release(dconn);
      }
      vibe_queue_consumer_release(dc, &ctx->dirty_queue);
    }

    if (batch.head) vibe_queue_injector_push(&ctx->inbox, &batch);
  }
  return NULL;
}

vibe_hub_t* vibe_open(void) {
  vibe_hub_t* ctx = calloc(1, sizeof(vibe_hub_t));
  ctx->running = true;
  ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  ctx->notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

  struct epoll_event ev = {.events = EPOLLIN | EPOLLET,
                           .data.ptr = &vibe_g_notify_sentinel};
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->notify_fd, &ev) < 0) {
    perror("epoll_ctl notify_fd");
  }
  pthread_create(&ctx->io_thread, NULL, vibe_io_loop, ctx);
  return ctx;
}

void vibe_close(vibe_hub_t* ctx) {
  ctx->running = false;
  uint64_t one = 1;
  if (write(ctx->notify_fd, &one, 8)) {
  }
  pthread_join(ctx->io_thread, NULL);
  close(ctx->epoll_fd);
  close(ctx->notify_fd);
  free(ctx);
}

bool vibe_bind(vibe_hub_t* ctx, const char* addr) {
  struct sockaddr_un un;
  struct sockaddr_in in;
  int family = vibe_parse_addr(addr, &un, &in);
  int fd = socket(family, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (family == AF_INET) {
    if (bind(fd, (struct sockaddr*)&in, sizeof(in)) < 0) {
      close(fd);
      return false;
    }
  } else {
    unlink(un.sun_path);
    if (bind(fd, (struct sockaddr*)&un, sizeof(un)) < 0) {
      close(fd);
      return false;
    }
  }
  if (listen(fd, 512) < 0) {
    close(fd);
    return false;
  }
  vibe_set_nonblocking(fd);

  vibe_conn_t* conn = calloc(1, sizeof(vibe_conn_t));
  conn->fd = fd;
  conn->active = true;
  conn->is_listener = true;
  conn->ctx = ctx;
  atomic_store(&conn->ref_count, 1);
  struct epoll_event ev = {.events = EPOLLIN, .data.ptr = conn};
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    close(fd);
    free(conn);
    return false;
  }
  return true;
}

void vibe_connect(vibe_hub_t* ctx, const char* addr) {
  struct sockaddr_un un;
  struct sockaddr_in in;
  int family = vibe_parse_addr(addr, &un, &in);
  int fd = socket(family, SOCK_STREAM, 0);
  vibe_tune_socket(fd, family);
  int ret;
  if (family == AF_INET)
    ret = connect(fd, (struct sockaddr*)&in, sizeof(in));
  else
    ret = connect(fd, (struct sockaddr*)&un, sizeof(un));

  if (ret < 0 && errno != EINPROGRESS) {
    close(fd);
    return;
  }

  vibe_conn_t* conn = calloc(1, sizeof(vibe_conn_t));
  conn->fd = fd;
  conn->active = false;
  conn->ctx = ctx;
  atomic_store(&conn->ref_count, 1);

  struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP,
                           .data.ptr = conn};
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    close(fd);
    free(conn);
    return;
  }
}

void vibe_disconnect(vibe_conn_t* conn) {
  if (conn) {
    conn->active = false;
    if (conn->fd >= 0) shutdown(conn->fd, SHUT_RDWR);
    vibe_conn_release(conn);
  }
}

bool vibe_cast(vibe_conn_t** conns, int count, const void* data, uint32_t len) {
  if (count == 0) return true;

  vibe_chunk_t* chunk = malloc(sizeof(vibe_chunk_t) + len);
  if (!chunk) return false;

  chunk->len = len;
  memcpy(chunk->data, data, len);

  atomic_store(&chunk->ref_count, 0);
  int success_count = 0;

  for (int i = 0; i < count; i++) {
    vibe_conn_t* c = conns[i];

    size_t pending =
        atomic_load_explicit(&c->pending_bytes, memory_order_relaxed);
    if (pending > VIBE_MAX_PENDING_BYTES) {
      continue;
    }

    atomic_fetch_add_explicit(&c->pending_bytes, len, memory_order_relaxed);

    vibe_node_t* node = vibe_alloc_node(0);
    node->type = VIBE_EVT_DATA;
    node->out.chunk = chunk;
    node->out.header_off = 0;
    node->out.payload_off = 0;

    struct vibe_queue_list l = {.head = &node->qnode, .tail = &node->qnode};
    vibe_queue_node_set_next(&node->qnode, NULL);

    vibe_queue_injector_push(&c->outbox, &l);
    vibe_schedule_dirty(c);

    success_count++;
  }

  if (success_count > 0) {
    atomic_fetch_add(&chunk->ref_count, success_count);
    return true;
  } else {
    free(chunk);
    return false;
  }
}

bool vibe_send(vibe_conn_t* conn, const void* data, uint32_t len) {
  return vibe_cast(&conn, 1, data, len);
}

vibe_msg_t* vibe_recv(vibe_hub_t* ctx) {
  vibe_queue_consumer c = vibe_queue_consumer_acquire(&ctx->inbox);
  if (!c) return NULL;

  struct vibe_queue_node* n;
  vibe_node_t *head = NULL, *tail = NULL;
  int limit = 256;

  while (limit-- > 0 && (n = vibe_queue_consumer_pop(&c, &ctx->inbox))) {
    vibe_node_t* node = VIBE_CONTAINER_OF(vibe_node_t, qnode, n);

    if (tail) {
      tail->next_msg = (struct vibe_node*)&node->next_msg;
    } else {
      head = node;
    }
    tail = node;
  }

  vibe_queue_consumer_release(c, &ctx->inbox);

  if (!head) return NULL;

  tail->next_msg = NULL;
  return (vibe_msg_t*)&head->next_msg;
}

void vibe_drop(vibe_msg_t* msg) {
  while (msg) {
    vibe_node_t* node =
        (vibe_node_t*)((char*)msg - offsetof(vibe_node_t, next_msg));
    vibe_msg_t* next = msg->next;
    if (node->conn) vibe_conn_release(node->conn);
    vibe_free_node(node);
    msg = next;
  }
}

#endif /* VIBE_IMPLEMENTATION_DONE */
#endif /* VIBE_IMPLEMENTATION */
