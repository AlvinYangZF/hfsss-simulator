#include "ftl/ftl_worker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sched.h>
#include <unistd.h>

/* Background thread entry points (defined in gc_thread.c / wl_thread.c) */
extern void *gc_thread_main(void *arg);
extern void *wl_thread_main(void *arg);

/* Global pointers set by ftl_mt_start() so worker threads can signal GC */
pthread_mutex_t *ftl_mt_gc_mutex_ptr = NULL;
pthread_cond_t  *ftl_mt_gc_cond_ptr  = NULL;

static void *ftl_worker_main(void *arg)
{
    struct ftl_worker *w = (struct ftl_worker *)arg;
    struct io_request req;
    struct io_completion cpl;

    while (w->running) {
        if (!io_ring_pop(&w->request_ring, &req)) {
            pthread_mutex_lock(&w->request_lock);
            while (w->running && io_ring_is_empty(&w->request_ring)) {
                pthread_cond_wait(&w->request_cond, &w->request_lock);
            }
            pthread_mutex_unlock(&w->request_lock);
            continue;
        }

        if (req.opcode == IO_OP_STOP) {
            w->running = false;
            break;
        }

        int rc = HFSSS_OK;

        switch (req.opcode) {
        case IO_OP_READ: {
            u32 page_size = w->ftl->config.page_size;
            u32 lba_step = req.lba_stride ? req.lba_stride : 1;
            u32 data_step = req.data_stride ? req.data_stride : page_size;
            u8 *ptr = req.data;
            for (u32 i = 0; i < req.count; i++) {
                rc = ftl_read_page_mt(w->ftl, w->taa,
                                      req.lba + (u64)i * lba_step, ptr);
                if (rc != HFSSS_OK) break;
                ptr += data_step;
            }
            break;
        }
        case IO_OP_WRITE: {
            u32 page_size = w->ftl->config.page_size;
            u32 lba_step = req.lba_stride ? req.lba_stride : 1;
            u32 data_step = req.data_stride ? req.data_stride : page_size;
            const u8 *ptr = req.data;
            for (u32 i = 0; i < req.count; i++) {
                rc = ftl_write_page_mt(w->ftl, w->taa,
                                       req.lba + (u64)i * lba_step, ptr);
                if (rc != HFSSS_OK) break;
                ptr += data_step;
            }
            break;
        }
        case IO_OP_TRIM: {
            u32 lba_step = req.lba_stride ? req.lba_stride : 1;
            for (u32 i = 0; i < req.count; i++) {
                rc = ftl_trim_page_mt(w->ftl, w->taa,
                                      req.lba + (u64)i * lba_step);
                if (rc != HFSSS_OK) break;
            }
            break;
        }
        case IO_OP_FLUSH:
            rc = HFSSS_OK;
            break;
        default:
            rc = HFSSS_ERR_INVAL;
            break;
        }

        cpl.nbd_handle = req.nbd_handle;
        cpl.data_offset = req.data_offset;
        cpl.byte_len = req.byte_len;
        cpl.status = rc;
        w->ops_completed++;
        if (rc != HFSSS_OK) w->ops_failed++;

        while (!io_ring_push(&w->completion_ring, &cpl)) {
            sched_yield();
        }
    }

    return NULL;
}

int ftl_mt_init(struct ftl_mt_ctx *ctx, struct ftl_config *config,
                struct hal_ctx *hal)
{
    int ret;

    if (!ctx || !config || !hal) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Initialize base FTL */
    ret = ftl_init(&ctx->ftl, config, hal);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize TAA with 256 shards */
    u64 total_pages = (u64)config->channel_count *
                      config->chips_per_channel *
                      config->dies_per_chip *
                      config->planes_per_die *
                      config->blocks_per_plane *
                      config->pages_per_block;

    ret = taa_init(&ctx->taa, config->total_lbas, total_pages,
                   TAA_DEFAULT_SHARDS);
    if (ret != HFSSS_OK) {
        ftl_cleanup(&ctx->ftl);
        return ret;
    }

    /* Initialize worker contexts (threads not started yet) */
    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        struct ftl_worker *w = &ctx->workers[i];
        w->worker_id = (u32)i;
        w->ftl = &ctx->ftl;
        w->taa = &ctx->taa;
        w->running = false;
        w->ops_completed = 0;
        w->ops_failed = 0;
        w->request_sync_initialized = false;

        ret = io_ring_init(&w->request_ring, sizeof(struct io_request),
                           IO_RING_DEFAULT_CAPACITY);
        if (ret != HFSSS_OK) goto fail;

        ret = io_ring_init(&w->completion_ring, sizeof(struct io_completion),
                           IO_RING_DEFAULT_CAPACITY);
        if (ret != HFSSS_OK) goto fail;

        ret = pthread_mutex_init(&w->request_lock, NULL);
        if (ret != 0) {
            ret = HFSSS_ERR_INVAL;
            goto fail;
        }

        ret = pthread_cond_init(&w->request_cond, NULL);
        if (ret != 0) {
            pthread_mutex_destroy(&w->request_lock);
            ret = HFSSS_ERR_INVAL;
            goto fail;
        }
        w->request_sync_initialized = true;
    }

    ctx->initialized = true;
    return HFSSS_OK;

fail:
    ftl_mt_cleanup(ctx);
    return ret;
}

void ftl_mt_cleanup(struct ftl_mt_ctx *ctx)
{
    if (!ctx) return;

    ftl_mt_stop(ctx);

    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        io_ring_cleanup(&ctx->workers[i].request_ring);
        io_ring_cleanup(&ctx->workers[i].completion_ring);
        if (ctx->workers[i].request_sync_initialized) {
            pthread_mutex_destroy(&ctx->workers[i].request_lock);
            pthread_cond_destroy(&ctx->workers[i].request_cond);
        }
    }

    taa_cleanup(&ctx->taa);
    ftl_cleanup(&ctx->ftl);
    memset(ctx, 0, sizeof(*ctx));
}

/* Tear down already-started workers on partial-startup failure.
 * Mirrors ftl_mt_stop(): push STOP, then signal request_cond under
 * the lock before joining, so workers blocked in pthread_cond_wait()
 * actually wake up and see the stop message. Without the signal the
 * join will deadlock. */
static void rollback_started_workers(struct ftl_mt_ctx *ctx, int count)
{
    for (int j = 0; j < count; j++) {
        struct io_request stop;
        memset(&stop, 0, sizeof(stop));
        stop.opcode = IO_OP_STOP;
        while (!io_ring_push(&ctx->workers[j].request_ring, &stop)) {
            sched_yield();
        }
        pthread_mutex_lock(&ctx->workers[j].request_lock);
        pthread_cond_signal(&ctx->workers[j].request_cond);
        pthread_mutex_unlock(&ctx->workers[j].request_lock);
        pthread_join(ctx->workers[j].thread, NULL);
        ctx->workers[j].running = false;
    }
}

int ftl_mt_start(struct ftl_mt_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        ctx->workers[i].running = true;
        int ret = pthread_create(&ctx->workers[i].thread, NULL,
                                  ftl_worker_main, &ctx->workers[i]);
        if (ret != 0) {
            ctx->workers[i].running = false;
            rollback_started_workers(ctx, i);
            return HFSSS_ERR_INVAL;
        }
    }

    /* Start GC background thread */
    pthread_mutex_init(&ctx->gc_mutex, NULL);
    pthread_cond_init(&ctx->gc_cond, NULL);
    ctx->gc_running = true;
    int gc_rc = pthread_create(&ctx->gc_thread, NULL, gc_thread_main, ctx);
    if (gc_rc != 0) {
        /* GC thread failed to start: clean up cond/mutex, unwind workers */
        ctx->gc_running = false;
        pthread_cond_destroy(&ctx->gc_cond);
        pthread_mutex_destroy(&ctx->gc_mutex);
        rollback_started_workers(ctx, FTL_NUM_WORKERS);
        return HFSSS_ERR_INVAL;
    }
    ftl_mt_gc_mutex_ptr = &ctx->gc_mutex;
    ftl_mt_gc_cond_ptr  = &ctx->gc_cond;

    /* Start WL/Read Disturb background thread */
    ctx->wl_running = true;
    int wl_rc = pthread_create(&ctx->wl_thread, NULL, wl_thread_main, ctx);
    if (wl_rc != 0) {
        /* WL failed: stop GC first (signal cond to unblock), then workers */
        ctx->wl_running = false;
        ctx->gc_running = false;
        pthread_mutex_lock(&ctx->gc_mutex);
        pthread_cond_signal(&ctx->gc_cond);
        pthread_mutex_unlock(&ctx->gc_mutex);
        pthread_join(ctx->gc_thread, NULL);
        ftl_mt_gc_mutex_ptr = NULL;
        ftl_mt_gc_cond_ptr  = NULL;
        pthread_cond_destroy(&ctx->gc_cond);
        pthread_mutex_destroy(&ctx->gc_mutex);
        rollback_started_workers(ctx, FTL_NUM_WORKERS);
        return HFSSS_ERR_INVAL;
    }

    return HFSSS_OK;
}

void ftl_mt_stop(struct ftl_mt_ctx *ctx)
{
    if (!ctx) return;

    /* Stop GC thread */
    if (ctx->gc_running) {
        ctx->gc_running = false;
        pthread_cond_signal(&ctx->gc_cond);
        pthread_join(ctx->gc_thread, NULL);
        ftl_mt_gc_mutex_ptr = NULL;
        ftl_mt_gc_cond_ptr  = NULL;
        pthread_mutex_destroy(&ctx->gc_mutex);
        pthread_cond_destroy(&ctx->gc_cond);
    }

    /* Stop WL thread */
    if (ctx->wl_running) {
        ctx->wl_running = false;
        pthread_join(ctx->wl_thread, NULL);
    }

    /* Stop worker threads */
    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        if (ctx->workers[i].running) {
            struct io_request stop;
            memset(&stop, 0, sizeof(stop));
            stop.opcode = IO_OP_STOP;
            while (!io_ring_push(&ctx->workers[i].request_ring, &stop)) {
                sched_yield();
            }
            pthread_mutex_lock(&ctx->workers[i].request_lock);
            pthread_cond_signal(&ctx->workers[i].request_cond);
            pthread_mutex_unlock(&ctx->workers[i].request_lock);
            pthread_join(ctx->workers[i].thread, NULL);
            ctx->workers[i].running = false;
        }
    }
}

bool ftl_mt_submit(struct ftl_mt_ctx *ctx, const struct io_request *req)
{
    if (!ctx || !req) return false;

    /* Route by LBA to worker: worker_id = lba % NUM_WORKERS */
    u32 wid = (u32)(req->lba % FTL_NUM_WORKERS);
    if (!io_ring_push(&ctx->workers[wid].request_ring, req)) {
        return false;
    }

    pthread_mutex_lock(&ctx->workers[wid].request_lock);
    pthread_cond_signal(&ctx->workers[wid].request_cond);
    pthread_mutex_unlock(&ctx->workers[wid].request_lock);
    return true;
}

bool ftl_mt_poll_completion(struct ftl_mt_ctx *ctx,
                            struct io_completion *out)
{
    if (!ctx || !out) return false;

    /* Round-robin poll all workers for completions */
    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        if (io_ring_pop(&ctx->workers[i].completion_ring, out)) {
            return true;
        }
    }
    return false;
}
