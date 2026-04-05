#ifndef __HFSSS_NBD_ASYNC_H
#define __HFSSS_NBD_ASYNC_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>

#define NBD_ASYNC_MAX_SLOTS    256
#define NBD_ASYNC_SLOT_BUFSZ   (64 * 4096)  /* 256 KB max per request */

enum slot_state {
    SLOT_FREE      = 0,
    SLOT_SUBMITTED = 1,
    SLOT_COMPLETE  = 2,
};

struct inflight_slot {
    _Atomic int       state;
    _Atomic uint32_t  pending_reqs;
    uint32_t          slot_id;      /* index in pool */
    uint64_t          nbd_handle;   /* echoed back to NBD client */
    uint16_t          nbd_cmd;      /* NBD_CMD_READ / WRITE / TRIM / FLUSH */
    uint32_t          byte_off;     /* sub-page offset for READ slicing */
    uint32_t          length;       /* original NBD request length */
    uint32_t          total_reqs;   /* Number of worker requests issued */
    int               completion_status;
    int               status;       /* FTL result code */
    uint8_t           data[NBD_ASYNC_SLOT_BUFSZ];
};

struct inflight_pool {
    struct inflight_slot *slots;
    uint32_t              capacity;
    _Atomic uint32_t      alloc_cursor;
    _Atomic uint32_t      in_use;
};

int  inflight_pool_init(struct inflight_pool *pool, uint32_t capacity);
void inflight_pool_cleanup(struct inflight_pool *pool);
struct inflight_slot *inflight_alloc(struct inflight_pool *pool);
void                  inflight_free(struct inflight_pool *pool, struct inflight_slot *slot);
struct inflight_slot *inflight_get(struct inflight_pool *pool, uint32_t slot_id);

struct ftl_mt_ctx;

struct nbd_async_ctx {
    int                   client_fd;
    uint32_t              lba_size;
    struct inflight_pool  pool;
    struct ftl_mt_ctx    *mt;
    volatile bool         running;
    pthread_t             sq_thread;
    pthread_t             cq_thread;
};

int  nbd_async_init(struct nbd_async_ctx *ctx, int client_fd,
                    uint32_t lba_size, struct ftl_mt_ctx *mt,
                    uint32_t pool_capacity);
void nbd_async_cleanup(struct nbd_async_ctx *ctx);
int  nbd_async_start(struct nbd_async_ctx *ctx);
void nbd_async_stop(struct nbd_async_ctx *ctx);

#endif /* __HFSSS_NBD_ASYNC_H */
