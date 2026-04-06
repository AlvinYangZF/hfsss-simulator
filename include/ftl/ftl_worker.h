#ifndef __HFSSS_FTL_WORKER_H
#define __HFSSS_FTL_WORKER_H

#include "common/common.h"
#include "ftl/ftl.h"
#include "ftl/taa.h"
#include "ftl/io_queue.h"
#include <pthread.h>
#include <stdatomic.h>

/* Match the default 4-channel / 2-plane geometry so aligned sequential I/O
 * can fan out across channel+plane targets instead of only channel targets.
 */
#define FTL_NUM_WORKERS 8

/* FTL Worker Context — one per worker thread */
struct ftl_worker {
    u32             worker_id;
    pthread_t       thread;
    struct io_ring  request_ring;    /* SPSC: dispatch → worker */
    struct io_ring  completion_ring; /* SPSC: worker → dispatch */
    pthread_mutex_t request_lock;
    pthread_cond_t  request_cond;
    bool            request_sync_initialized;
    struct ftl_ctx *ftl;
    struct taa_ctx *taa;
    atomic_bool     running;
    u64             ops_completed;
    u64             ops_failed;
};

/* Multi-threaded FTL controller */
struct ftl_mt_ctx {
    struct ftl_ctx     ftl;
    struct taa_ctx     taa;
    struct ftl_worker  workers[FTL_NUM_WORKERS];
    bool               initialized;

    /* Background GC thread */
    pthread_t          gc_thread;
    pthread_mutex_t    gc_mutex;
    pthread_cond_t     gc_cond;
    atomic_bool        gc_running;

    /* Background WL/Read Disturb thread */
    pthread_t          wl_thread;
    atomic_bool        wl_running;
};

/* Lifecycle */
int  ftl_mt_init(struct ftl_mt_ctx *ctx, struct ftl_config *config,
                 struct hal_ctx *hal);
void ftl_mt_cleanup(struct ftl_mt_ctx *ctx);

/* Start/stop worker threads */
int  ftl_mt_start(struct ftl_mt_ctx *ctx);
void ftl_mt_stop(struct ftl_mt_ctx *ctx);

/* Async I/O submission (from NBD dispatch thread) */
bool ftl_mt_submit(struct ftl_mt_ctx *ctx, const struct io_request *req);

/* Poll completions (from NBD dispatch thread) */
bool ftl_mt_poll_completion(struct ftl_mt_ctx *ctx,
                            struct io_completion *out);

#endif /* __HFSSS_FTL_WORKER_H */
