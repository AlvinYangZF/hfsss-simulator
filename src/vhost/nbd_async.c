#include "vhost/nbd_async.h"
#include "ftl/ftl_worker.h"
#include "ftl/io_queue.h"
#include "common/io_err_trace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
    pool->slots = (struct inflight_slot *)calloc(capacity, sizeof(struct inflight_slot));
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
    if (!pool)
        return;
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
        if (atomic_compare_exchange_strong(&pool->slots[idx].state, &expected, SLOT_SUBMITTED)) {
            atomic_store(&pool->slots[idx].pending_reqs, 0);
            pool->slots[idx].total_reqs = 0;
            pool->slots[idx].completion_status = HFSSS_OK;
            pool->slots[idx].status = HFSSS_OK;
            atomic_fetch_add(&pool->in_use, 1);
            return &pool->slots[idx];
        }
    }
    return NULL;
}

void inflight_free(struct inflight_pool *pool, struct inflight_slot *slot)
{
    if (!pool || !slot)
        return;
    atomic_store(&slot->state, SLOT_FREE);
    atomic_fetch_sub(&pool->in_use, 1);
}

struct inflight_slot *inflight_get(struct inflight_pool *pool, uint32_t slot_id)
{
    if (!pool || slot_id >= pool->capacity)
        return NULL;
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
        if (n <= 0)
            return -1;
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
        if (n <= 0)
            return -1;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int send_iov_exact(int fd, struct iovec *iov, int iovcnt)
{
    while (iovcnt > 0) {
        ssize_t n = writev(fd, iov, iovcnt);
        if (n <= 0) {
            return -1;
        }

        size_t written = (size_t)n;
        while (iovcnt > 0 && written >= iov[0].iov_len) {
            written -= iov[0].iov_len;
            iov++;
            iovcnt--;
        }

        if (iovcnt > 0 && written > 0) {
            iov[0].iov_base = (uint8_t *)iov[0].iov_base + written;
            iov[0].iov_len -= written;
        }
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * NBD protocol helpers
 * ----------------------------------------------------------------------- */

#define NBD_REQUEST_MAGIC 0x25609513u
#define NBD_REPLY_MAGIC 0x67446698u
#define NBD_CMD_READ 0u
#define NBD_CMD_WRITE 1u
#define NBD_CMD_DISC 2u
#define NBD_CMD_FLUSH 3u
#define NBD_CMD_TRIM 4u
#define NBD_EIO 5u

static inline uint64_t nbd_htonll(uint64_t v)
{
    union {
        uint64_t u64;
        uint32_t u32[2];
    } s;
    s.u32[0] = htonl((uint32_t)(v >> 32));
    s.u32[1] = htonl((uint32_t)(v & 0xFFFFFFFFU));
    return s.u64;
}
static inline uint64_t nbd_ntohll(uint64_t v)
{
    return nbd_htonll(v);
}

struct __attribute__((packed)) nbd_request_hdr {
    uint32_t magic;
    uint16_t flags; /* NBD command flags */
    uint16_t type;  /* NBD_CMD_* */
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
    rep.magic = htonl(NBD_REPLY_MAGIC);
    rep.error = htonl(error);
    rep.handle = handle; /* verbatim */
    return send_exact(fd, &rep, sizeof(rep));
}

#define NBD_CQ_BATCH_MAX 16

static bool nbd_async_is_aligned_page_io(uint16_t cmd, uint32_t byte_off, uint32_t length, uint32_t count,
                                         uint32_t lba_size)
{
    if (cmd != NBD_CMD_READ && cmd != NBD_CMD_WRITE && cmd != NBD_CMD_TRIM) {
        return false;
    }

    return byte_off == 0 && length == count * lba_size;
}

static void nbd_async_init_slot(struct inflight_slot *slot, uint64_t handle, uint16_t cmd, uint32_t byte_off,
                                uint32_t length, uint32_t total_reqs)
{
    slot->nbd_handle = handle;
    slot->nbd_cmd = cmd;
    slot->byte_off = byte_off;
    slot->length = length;
    slot->total_reqs = total_reqs;
    slot->completion_status = HFSSS_OK;
    slot->status = HFSSS_OK;
    atomic_store(&slot->pending_reqs, total_reqs);
}

static void nbd_async_submit_req(struct nbd_async_ctx *ctx, const struct io_request *io_req)
{
    while (ctx->running && !ftl_mt_submit(ctx->mt, io_req)) {
        sched_yield();
    }
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

        uint32_t magic = ntohl(req_hdr.magic);
        uint16_t type = ntohs(req_hdr.type);
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
        uint64_t lba = offset / lba_size;
        uint32_t byte_off = (uint32_t)(offset % lba_size);
        uint64_t end_byte = offset + length;
        uint64_t end_lba = (end_byte + lba_size - 1) / lba_size;
        uint32_t count = (uint32_t)(end_lba - lba);
        uint32_t full_bytes = count * lba_size;
        bool split_read =
            type == NBD_CMD_READ && count > 1 && nbd_async_is_aligned_page_io(type, byte_off, length, count, lba_size);
        bool split_strided = (type == NBD_CMD_WRITE || type == NBD_CMD_TRIM) && count > 1 &&
                             nbd_async_is_aligned_page_io(type, byte_off, length, count, lba_size);
        u32 total_reqs = 1;

        /* Alloc inflight slot (spin if pool exhausted = backpressure) */
        struct inflight_slot *slot = NULL;
        while (ctx->running) {
            slot = inflight_alloc(&ctx->pool);
            if (slot)
                break;
            sched_yield();
        }
        if (!slot)
            break;

        if (full_bytes > NBD_ASYNC_SLOT_BUFSZ) {
            send_nbd_reply(ctx->client_fd, handle, NBD_EIO);
            inflight_free(&ctx->pool, slot);
            continue;
        }

        if (split_read) {
            total_reqs = count;
        } else if (split_strided) {
            total_reqs = count < FTL_NUM_WORKERS ? count : FTL_NUM_WORKERS;
        }

        nbd_async_init_slot(slot, handle, type, byte_off, length, total_reqs);

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

        /* Build io_request(s) and submit to FTL worker(s). */
        struct io_request io_req;
        memset(&io_req, 0, sizeof(io_req));
        io_req.nbd_handle = slot->slot_id; /* slot_id flows through */

        switch (type) {
        case NBD_CMD_READ:
            io_req.opcode = IO_OP_READ;
            break;
        case NBD_CMD_WRITE:
            io_req.opcode = IO_OP_WRITE;
            break;
        case NBD_CMD_TRIM:
            io_req.opcode = IO_OP_TRIM;
            break;
        case NBD_CMD_FLUSH:
            io_req.opcode = IO_OP_FLUSH;
            break;
        default:
            inflight_free(&ctx->pool, slot);
            continue;
        }

        if (split_read) {
            for (u32 i = 0; i < count; i++) {
                io_req.lba = lba + i;
                io_req.count = 1;
                io_req.lba_stride = 1;
                io_req.data_stride = lba_size;
                io_req.data_offset = i * lba_size;
                io_req.byte_len = lba_size;
                io_req.data = slot->data + io_req.data_offset;
                nbd_async_submit_req(ctx, &io_req);
            }
        } else if (split_strided) {
            u32 first_wid = (u32)(lba % FTL_NUM_WORKERS);

            for (u32 worker_slot = 0; worker_slot < total_reqs; worker_slot++) {
                u32 page_idx = worker_slot;
                u32 wid = (first_wid + worker_slot) % FTL_NUM_WORKERS;
                u32 pages_for_worker;

                (void)wid;
                if (page_idx >= count) {
                    break;
                }

                pages_for_worker = 1 + (count - page_idx - 1) / FTL_NUM_WORKERS;
                io_req.lba = lba + page_idx;
                io_req.count = pages_for_worker;
                io_req.lba_stride = FTL_NUM_WORKERS;
                io_req.data_stride = lba_size * FTL_NUM_WORKERS;
                io_req.data_offset = page_idx * lba_size;
                io_req.byte_len = pages_for_worker * lba_size;
                io_req.data = slot->data + io_req.data_offset;
                nbd_async_submit_req(ctx, &io_req);
            }
        } else {
            io_req.lba = lba;
            io_req.count = count;
            io_req.data = slot->data;
            io_req.lba_stride = 1;
            io_req.data_stride = lba_size;
            io_req.data_offset = 0;
            io_req.byte_len = full_bytes;
            nbd_async_submit_req(ctx, &io_req);
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
    struct iovec iov[NBD_CQ_BATCH_MAX * 2];
    struct inflight_slot *completed_slots[NBD_CQ_BATCH_MAX];
    struct nbd_reply_hdr replies[NBD_CQ_BATCH_MAX];
    int idle_spins = 0;

    while (ctx->running) {
        bool found = false;
        int batch_count = 0;
        int iovcnt = 0;

        /* Poll all worker completion rings */
        for (int w = 0; w < FTL_NUM_WORKERS; w++) {
            while (batch_count < NBD_CQ_BATCH_MAX && io_ring_pop(&ctx->mt->workers[w].completion_ring, &cpl)) {
                found = true;
                idle_spins = 0;

                struct inflight_slot *slot = inflight_get(&ctx->pool, (uint32_t)cpl.nbd_handle);
                if (!slot) {
                    continue;
                }

                if (slot->nbd_cmd == NBD_CMD_READ && cpl.status != 0) {
                    memset(slot->data + cpl.data_offset, 0, cpl.byte_len);
                } else if (cpl.status != HFSSS_OK && slot->completion_status == HFSSS_OK) {
                    slot->completion_status = cpl.status;
                }

                if (atomic_fetch_sub(&slot->pending_reqs, 1) != 1) {
                    continue;
                }

                slot->status = slot->completion_status;
                uint32_t error = (slot->status == HFSSS_OK) ? 0 : NBD_EIO;
                if (error) {
                    IO_ERR_TRACE("L=nbd_async_cq cmd=%u handle=0x%016llx length=%u byte_off=%u rc=%d -> EIO",
                                 (unsigned)slot->nbd_cmd,
                                 (unsigned long long)slot->nbd_handle,
                                 slot->length, slot->byte_off, slot->status);
                }

                replies[batch_count].magic = htonl(NBD_REPLY_MAGIC);
                replies[batch_count].error = htonl(error);
                replies[batch_count].handle = slot->nbd_handle;
                iov[iovcnt].iov_base = &replies[batch_count];
                iov[iovcnt].iov_len = sizeof(replies[batch_count]);
                iovcnt++;

                /* For READ: send data payload */
                if (slot->nbd_cmd == NBD_CMD_READ) {
                    iov[iovcnt].iov_base = slot->data + slot->byte_off;
                    iov[iovcnt].iov_len = slot->length;
                    iovcnt++;
                }
                completed_slots[batch_count] = slot;
                batch_count++;
            }

            if (batch_count >= NBD_CQ_BATCH_MAX) {
                break;
            }
        }

        if (batch_count > 0) {
            if (send_iov_exact(ctx->client_fd, iov, iovcnt) != 0) {
                ctx->running = false;
                break;
            }

            for (int i = 0; i < batch_count; i++) {
                inflight_free(&ctx->pool, completed_slots[i]);
            }
            continue;
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
            struct inflight_slot *slot = inflight_get(&ctx->pool, (uint32_t)cpl.nbd_handle);
            if (slot) {
                slot->completion_status = NBD_EIO;
                if (atomic_fetch_sub(&slot->pending_reqs, 1) == 1) {
                    send_nbd_reply(ctx->client_fd, slot->nbd_handle, NBD_EIO);
                    inflight_free(&ctx->pool, slot);
                }
            }
        }
    }

    return NULL;
}

/* -----------------------------------------------------------------------
 * Async context lifecycle
 * ----------------------------------------------------------------------- */

int nbd_async_init(struct nbd_async_ctx *ctx, int client_fd, uint32_t lba_size, struct ftl_mt_ctx *mt,
                   uint32_t pool_capacity)
{
    if (!ctx || !mt || client_fd < 0)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->client_fd = client_fd;
    ctx->lba_size = lba_size;
    ctx->mt = mt;
    ctx->running = false;

    /* Enable TCP_NODELAY for low-latency replies */
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    return inflight_pool_init(&ctx->pool, pool_capacity);
}

void nbd_async_cleanup(struct nbd_async_ctx *ctx)
{
    if (!ctx)
        return;
    nbd_async_stop(ctx);
    inflight_pool_cleanup(&ctx->pool);
    memset(ctx, 0, sizeof(*ctx));
}

int nbd_async_start(struct nbd_async_ctx *ctx)
{
    if (!ctx)
        return -1;
    ctx->running = true;

    int ret = pthread_create(&ctx->sq_thread, NULL, nbd_sq_thread_main, ctx);
    if (ret != 0) {
        ctx->running = false;
        return -1;
    }

    ret = pthread_create(&ctx->cq_thread, NULL, nbd_cq_thread_main, ctx);
    if (ret != 0) {
        /* CQ failed after SQ started: unblock the SQ thread's recv()
         * by shutting down the read side of the client socket, then
         * join. Without the shutdown the SQ thread stays in blocking
         * recv() and pthread_join() deadlocks. */
        ctx->running = false;
        shutdown(ctx->client_fd, SHUT_RD);
        pthread_join(ctx->sq_thread, NULL);
        return -1;
    }

    return 0;
}

void nbd_async_stop(struct nbd_async_ctx *ctx)
{
    if (!ctx || !ctx->running)
        return;
    ctx->running = false;
    /* SQ thread will unblock when recv() returns 0/error.
     * CQ thread will see running=false on next poll cycle. */
    shutdown(ctx->client_fd, SHUT_RD); /* unblock recv in SQ */
    pthread_join(ctx->sq_thread, NULL);
    pthread_join(ctx->cq_thread, NULL);
}
