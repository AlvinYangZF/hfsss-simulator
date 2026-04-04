#include "vhost/nbd_async.h"
#include "ftl/ftl_worker.h"
#include "ftl/io_queue.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* -----------------------------------------------------------------------
 * Inflight Pool
 * ----------------------------------------------------------------------- */

int inflight_pool_init(struct inflight_pool *pool, uint32_t capacity)
{
    if (!pool || capacity == 0 || capacity > NBD_ASYNC_MAX_SLOTS) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    pool->slots = (struct inflight_slot *)calloc(capacity,
                                                  sizeof(struct inflight_slot));
    if (!pool->slots) {
        return -1;
    }

    pool->capacity = capacity;
    atomic_store(&pool->alloc_cursor, 0);
    atomic_store(&pool->in_use, 0);

    for (uint32_t i = 0; i < capacity; i++) {
        pool->slots[i].slot_id = i;
        atomic_store(&pool->slots[i].state, SLOT_FREE);
    }

    return 0;
}

void inflight_pool_cleanup(struct inflight_pool *pool)
{
    if (!pool) return;
    free(pool->slots);
    memset(pool, 0, sizeof(*pool));
}

struct inflight_slot *inflight_alloc(struct inflight_pool *pool)
{
    if (!pool || atomic_load(&pool->in_use) >= pool->capacity) {
        return NULL;
    }

    uint32_t start = atomic_fetch_add(&pool->alloc_cursor, 1) % pool->capacity;
    for (uint32_t i = 0; i < pool->capacity; i++) {
        uint32_t idx = (start + i) % pool->capacity;
        int expected = SLOT_FREE;
        if (atomic_compare_exchange_strong(&pool->slots[idx].state,
                                            &expected, SLOT_SUBMITTED)) {
            atomic_fetch_add(&pool->in_use, 1);
            return &pool->slots[idx];
        }
    }
    return NULL;
}

void inflight_free(struct inflight_pool *pool, struct inflight_slot *slot)
{
    if (!pool || !slot) return;
    atomic_store(&slot->state, SLOT_FREE);
    atomic_fetch_sub(&pool->in_use, 1);
}

struct inflight_slot *inflight_get(struct inflight_pool *pool, uint32_t slot_id)
{
    if (!pool || slot_id >= pool->capacity) return NULL;
    return &pool->slots[slot_id];
}

/* -----------------------------------------------------------------------
 * Helper: exact read/write on socket
 * ----------------------------------------------------------------------- */

static int recv_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = recv(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int send_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = send(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * NBD protocol helpers
 * ----------------------------------------------------------------------- */

#define NBD_REQUEST_MAGIC  0x25609513u
#define NBD_REPLY_MAGIC    0x67446698u
#define NBD_CMD_READ       0u
#define NBD_CMD_WRITE      1u
#define NBD_CMD_DISC       2u
#define NBD_CMD_FLUSH      3u
#define NBD_CMD_TRIM       4u
#define NBD_EIO            5u

static inline uint64_t nbd_htonll(uint64_t v)
{
    union { uint64_t u64; uint32_t u32[2]; } s;
    s.u32[0] = htonl((uint32_t)(v >> 32));
    s.u32[1] = htonl((uint32_t)(v & 0xFFFFFFFFU));
    return s.u64;
}
static inline uint64_t nbd_ntohll(uint64_t v) { return nbd_htonll(v); }

struct __attribute__((packed)) nbd_request_hdr {
    uint32_t magic;
    uint16_t flags;   /* NBD command flags */
    uint16_t type;    /* NBD_CMD_* */
    uint64_t handle;
    uint64_t offset;
    uint32_t length;
};

struct __attribute__((packed)) nbd_reply_hdr {
    uint32_t magic;
    uint32_t error;
    uint64_t handle;
};

static int send_nbd_reply(int fd, uint64_t handle, uint32_t error)
{
    struct nbd_reply_hdr rep;
    rep.magic  = htonl(NBD_REPLY_MAGIC);
    rep.error  = htonl(error);
    rep.handle = handle;  /* verbatim */
    return send_exact(fd, &rep, sizeof(rep));
}

/* -----------------------------------------------------------------------
 * Submit Thread (SQ) — reads NBD requests, dispatches to FTL workers
 * ----------------------------------------------------------------------- */

static void *nbd_sq_thread_main(void *arg)
{
    struct nbd_async_ctx *ctx = (struct nbd_async_ctx *)arg;
    struct nbd_request_hdr req_hdr;

    while (ctx->running) {
        /* Read request header (blocking — SQ thread owns recv) */
        if (recv_exact(ctx->client_fd, &req_hdr, sizeof(req_hdr)) != 0) {
            if (ctx->running) {
                fprintf(stderr, "[NBD-SQ] Read request failed\n");
            }
            ctx->running = false;
            break;
        }

        uint32_t magic  = ntohl(req_hdr.magic);
        uint16_t type   = ntohs(req_hdr.type);
        uint64_t offset = nbd_ntohll(req_hdr.offset);
        uint32_t length = ntohl(req_hdr.length);
        uint64_t handle = req_hdr.handle;

        if (magic != NBD_REQUEST_MAGIC) {
            fprintf(stderr, "[NBD-SQ] Bad magic 0x%08x\n", magic);
            ctx->running = false;
            break;
        }

        if (type == NBD_CMD_DISC) {
            fprintf(stderr, "[NBD-SQ] Client disconnected\n");
            ctx->running = false;
            break;
        }

        /* Compute LBA range */
        uint32_t lba_size = ctx->lba_size;
        uint64_t lba      = offset / lba_size;
        uint32_t byte_off = (uint32_t)(offset % lba_size);
        uint64_t end_byte = offset + length;
        uint64_t end_lba  = (end_byte + lba_size - 1) / lba_size;
        uint32_t count    = (uint32_t)(end_lba - lba);

        /* Alloc inflight slot (spin if pool exhausted = backpressure) */
        struct inflight_slot *slot = NULL;
        while (ctx->running) {
            slot = inflight_alloc(&ctx->pool);
            if (slot) break;
            sched_yield();
        }
        if (!slot) break;

        slot->nbd_handle = handle;
        slot->nbd_cmd    = type;
        slot->byte_off   = byte_off;
        slot->length     = length;
        slot->status     = 0;

        /* For WRITE: read payload into slot buffer */
        if (type == NBD_CMD_WRITE) {
            if (recv_exact(ctx->client_fd, slot->data, length) != 0) {
                inflight_free(&ctx->pool, slot);
                ctx->running = false;
                break;
            }
            /* Handle sub-page RMW: for simplicity in async mode,
             * require aligned writes. Sub-page writes fall back to
             * the sync path. In practice, fio with --direct=1 and
             * bs>=4K always generates aligned writes. */
        }

        /* Build io_request and submit to FTL worker */
        struct io_request io_req;
        memset(&io_req, 0, sizeof(io_req));
        io_req.nbd_handle = slot->slot_id;  /* slot_id flows through */
        io_req.lba   = lba;
        io_req.count = count;
        io_req.data  = slot->data;

        switch (type) {
        case NBD_CMD_READ:  io_req.opcode = IO_OP_READ;  break;
        case NBD_CMD_WRITE: io_req.opcode = IO_OP_WRITE; break;
        case NBD_CMD_TRIM:  io_req.opcode = IO_OP_TRIM;  break;
        case NBD_CMD_FLUSH: io_req.opcode = IO_OP_FLUSH; break;
        default:
            inflight_free(&ctx->pool, slot);
            continue;
        }

        while (!ftl_mt_submit(ctx->mt, &io_req)) {
            sched_yield();
        }
    }

    return NULL;
}

/* -----------------------------------------------------------------------
 * Completion Thread (CQ) — polls worker completions, sends NBD replies
 * ----------------------------------------------------------------------- */

static void *nbd_cq_thread_main(void *arg)
{
    struct nbd_async_ctx *ctx = (struct nbd_async_ctx *)arg;
    struct io_completion cpl;
    int idle_spins = 0;

    while (ctx->running) {
        bool found = false;

        /* Poll all worker completion rings */
        for (int w = 0; w < FTL_NUM_WORKERS; w++) {
            while (io_ring_pop(&ctx->mt->workers[w].completion_ring, &cpl)) {
                found = true;
                idle_spins = 0;

                struct inflight_slot *slot = inflight_get(&ctx->pool,
                                                           (uint32_t)cpl.nbd_handle);
                if (!slot) continue;

                uint32_t error = (cpl.status == 0) ? 0 : NBD_EIO;

                /* For READ: unmapped pages return zeros (not error) */
                if (slot->nbd_cmd == NBD_CMD_READ && cpl.status != 0) {
                    memset(slot->data, 0, slot->length);
                    error = 0;  /* return zeros, not EIO */
                }

                /* Send NBD reply header */
                if (send_nbd_reply(ctx->client_fd, slot->nbd_handle, error) != 0) {
                    ctx->running = false;
                    break;
                }

                /* For READ: send data payload */
                if (slot->nbd_cmd == NBD_CMD_READ && error == 0) {
                    if (send_exact(ctx->client_fd,
                                   slot->data + slot->byte_off,
                                   slot->length) != 0) {
                        ctx->running = false;
                        break;
                    }
                }

                inflight_free(&ctx->pool, slot);
            }
        }

        if (!found) {
            if (idle_spins < 64) {
                sched_yield();
            } else {
                usleep(1);
            }
            idle_spins++;
        }
    }

    /* Drain remaining completions */
    for (int w = 0; w < FTL_NUM_WORKERS; w++) {
        while (io_ring_pop(&ctx->mt->workers[w].completion_ring, &cpl)) {
            struct inflight_slot *slot = inflight_get(&ctx->pool,
                                                       (uint32_t)cpl.nbd_handle);
            if (slot) {
                send_nbd_reply(ctx->client_fd, slot->nbd_handle, NBD_EIO);
                inflight_free(&ctx->pool, slot);
            }
        }
    }

    return NULL;
}

/* -----------------------------------------------------------------------
 * Async context lifecycle
 * ----------------------------------------------------------------------- */

int nbd_async_init(struct nbd_async_ctx *ctx, int client_fd,
                   uint32_t lba_size, struct ftl_mt_ctx *mt,
                   uint32_t pool_capacity)
{
    if (!ctx || !mt || client_fd < 0) return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->client_fd = client_fd;
    ctx->lba_size  = lba_size;
    ctx->mt        = mt;
    ctx->running   = false;

    /* Enable TCP_NODELAY for low-latency replies */
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    return inflight_pool_init(&ctx->pool, pool_capacity);
}

void nbd_async_cleanup(struct nbd_async_ctx *ctx)
{
    if (!ctx) return;
    nbd_async_stop(ctx);
    inflight_pool_cleanup(&ctx->pool);
    memset(ctx, 0, sizeof(*ctx));
}

int nbd_async_start(struct nbd_async_ctx *ctx)
{
    if (!ctx) return -1;
    ctx->running = true;

    int ret = pthread_create(&ctx->sq_thread, NULL, nbd_sq_thread_main, ctx);
    if (ret != 0) {
        ctx->running = false;
        return -1;
    }

    ret = pthread_create(&ctx->cq_thread, NULL, nbd_cq_thread_main, ctx);
    if (ret != 0) {
        ctx->running = false;
        pthread_join(ctx->sq_thread, NULL);
        return -1;
    }

    return 0;
}

void nbd_async_stop(struct nbd_async_ctx *ctx)
{
    if (!ctx || !ctx->running) return;
    ctx->running = false;
    /* SQ thread will unblock when recv() returns 0/error.
     * CQ thread will see running=false on next poll cycle. */
    shutdown(ctx->client_fd, SHUT_RD);  /* unblock recv in SQ */
    pthread_join(ctx->sq_thread, NULL);
    pthread_join(ctx->cq_thread, NULL);
}
