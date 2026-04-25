#include "controller/channel_worker.h"

#include <sched.h>
#include <string.h>

#include "media/media.h"

static int dispatch_one(struct channel_worker *w, struct channel_cmd *cmd)
{
    /* Refuse any command addressed to a channel this worker does not
     * own. Without this guard a worker for channel 0 would happily
     * drive media ops against channel 5, corrupting the per-channel
     * execution boundary that REQ-044 exists to establish. */
    if (cmd->ch != w->channel_id) {
        return HFSSS_ERR_INVAL;
    }

    switch (cmd->op) {
    case CHANNEL_CMD_READ:
        return media_nand_read(w->media, cmd->ch, cmd->chip, cmd->die, cmd->plane, cmd->block, cmd->page,
                               cmd->data_buf, cmd->spare_buf);
    case CHANNEL_CMD_PROGRAM:
        return media_nand_program(w->media, cmd->ch, cmd->chip, cmd->die, cmd->plane, cmd->block, cmd->page,
                                  cmd->data_buf, cmd->spare_buf);
    case CHANNEL_CMD_ERASE:
        return media_nand_erase(w->media, cmd->ch, cmd->chip, cmd->die, cmd->plane, cmd->block);
    default:
        return HFSSS_ERR_INVAL;
    }
}

static void complete_one(struct channel_worker *w, struct channel_cmd *cmd)
{
    /* REQ-045: stamp the completion timestamp BEFORE publishing done.
     * The release on done carries this store, so any reader that
     * observes done == 1 with acquire ordering also sees the final
     * complete_ts_ns. */
    cmd->complete_ts_ns = get_time_ns();

    /* Publish completion BEFORE invoking the user callback. This lets
     * a callback safely reclaim the command (including cb_ctx) without
     * racing the worker's subsequent publication of done. Waiters and
     * callbacks are therefore alternative completion paths: a caller
     * that both polls via channel_cmd_wait AND installs a reclaiming
     * callback is responsible for serialising them. */
    atomic_store_explicit(&cmd->done, 1, memory_order_release);
    atomic_fetch_add_explicit(&w->completed, 1, memory_order_relaxed);
    if (cmd->on_complete) {
        cmd->on_complete(cmd, cmd->cb_ctx);
    }
}

static void *channel_worker_loop(void *arg)
{
    struct channel_worker *w = (struct channel_worker *)arg;

    for (;;) {
        struct channel_cmd *cmd = NULL;
        int rc = spsc_ring_tryget(&w->ring, &cmd);
        if (rc == HFSSS_OK && cmd != NULL) {
            cmd->status = dispatch_one(w, cmd);
            complete_one(w, cmd);
            continue;
        }

        /* Ring is empty. Only exit once stop is set AND the ring stayed
         * empty on this pass — this drains anything the producer racily
         * pushed before observing stop, so no submitted command is left
         * orphaned when the worker shuts down. */
        if (atomic_load_explicit(&w->stop, memory_order_acquire)) {
            break;
        }
        sched_yield();
    }

    return NULL;
}

int channel_worker_init(struct channel_worker *w, u32 channel_id, struct media_ctx *media,
                        u32 ring_capacity, u32 cq_capacity)
{
    if (!w || !media || ring_capacity == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(w, 0, sizeof(*w));
    w->channel_id = channel_id;
    w->media = media;
    atomic_store(&w->stop, 0);
    atomic_store(&w->submitted, 0);
    atomic_store(&w->completed, 0);

    int rc = spsc_ring_init(&w->ring, (u32)sizeof(struct channel_cmd *), ring_capacity);
    if (rc != HFSSS_OK) {
        return rc;
    }

    /* REQ-045: opt-in completion queue. Allocated only when caller
     * asks for it. cq_capacity inherits the same power-of-two
     * requirement as ring_capacity; spsc_ring_init enforces it. */
    w->cq_enabled = false;
    if (cq_capacity > 0) {
        rc = spsc_ring_init(&w->cq, (u32)sizeof(struct channel_cmd *), cq_capacity);
        if (rc != HFSSS_OK) {
            spsc_ring_cleanup(&w->ring);
            return rc;
        }
        w->cq_enabled = true;
    }

    if (pthread_create(&w->thread, NULL, channel_worker_loop, w) != 0) {
        if (w->cq_enabled) {
            spsc_ring_cleanup(&w->cq);
        }
        spsc_ring_cleanup(&w->ring);
        return HFSSS_ERR_NOMEM;
    }
    w->thread_started = true;
    return HFSSS_OK;
}

void channel_worker_stop(struct channel_worker *w)
{
    if (!w) {
        return;
    }
    atomic_store_explicit(&w->stop, 1, memory_order_release);
}

void channel_worker_cleanup(struct channel_worker *w)
{
    if (!w) {
        return;
    }
    if (w->thread_started) {
        atomic_store_explicit(&w->stop, 1, memory_order_release);
        pthread_join(w->thread, NULL);
        w->thread_started = false;
    }
    if (w->cq_enabled) {
        spsc_ring_cleanup(&w->cq);
        w->cq_enabled = false;
    }
    spsc_ring_cleanup(&w->ring);
    memset(w, 0, sizeof(*w));
}

int channel_worker_submit(struct channel_worker *w, struct channel_cmd *cmd)
{
    if (!w || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    /* REQ-045: three-path mutual exclusion. CQ-enabled worker refuses
     * cmds that carry an on_complete callback — the CQ is the
     * delivery channel in that mode and a callback would race with
     * a CQ consumer that pops the same cmd. Reject before any
     * mutation so the caller can flip the cmd back to a non-CQ worker
     * if needed. */
    if (w->cq_enabled && cmd->on_complete != NULL) {
        return HFSSS_ERR_INVAL;
    }

    /* Reject submits once stop has been signalled. The acquire load
     * pairs with the release store in channel_worker_stop; together
     * they close the window in which a late submit could land in the
     * ring just as the consumer is exiting. A caller that races submit
     * against stop must coordinate externally — this check only shrinks
     * the window, it does not eliminate a pathological TOCTOU. */
    if (atomic_load_explicit(&w->stop, memory_order_acquire)) {
        return HFSSS_ERR_BUSY;
    }

    atomic_store_explicit(&cmd->done, 0, memory_order_relaxed);
    cmd->status = 0;

    /* REQ-045: producer-side timestamp + owner back-pointer set BEFORE
     * the ring put. The worker thread does an acquire load on the ring
     * tail when it dequeues; that pairs with the ring's release on
     * head and carries these stores. The owner pointer is read by
     * channel_cmd_wait for the CQ-mode wait rejection check. */
    cmd->submit_ts_ns  = get_time_ns();
    cmd->complete_ts_ns = 0;
    cmd->owner = w;

    int rc = spsc_ring_tryput(&w->ring, &cmd);
    if (rc != HFSSS_OK) {
        return HFSSS_ERR_BUSY;
    }
    atomic_fetch_add_explicit(&w->submitted, 1, memory_order_relaxed);
    return HFSSS_OK;
}

int channel_cmd_wait(struct channel_cmd *cmd)
{
    if (!cmd) {
        return HFSSS_ERR_INVAL;
    }
    /* REQ-045: refuse the legacy poll path when the cmd's owner is in
     * CQ mode. Reading owner is safe here because submit set it before
     * the ring put and we are by contract the only legitimate waiter.
     * The cmd still completes via the CQ — this only refuses the
     * caller's attempt to reach for the wrong reaping API. */
    struct channel_worker *owner = cmd->owner;
    if (owner != NULL && owner->cq_enabled) {
        return HFSSS_ERR_INVAL;
    }
    while (atomic_load_explicit(&cmd->done, memory_order_acquire) == 0) {
        sched_yield();
    }
    return cmd->status;
}

int channel_worker_drain(struct channel_worker *w, struct channel_cmd **buf, u32 buf_cap)
{
    if (!w || !buf || buf_cap == 0) {
        return HFSSS_ERR_INVAL;
    }
    if (!w->cq_enabled) {
        return HFSSS_ERR_INVAL;
    }
    /* REQ-045 Task 4 lands the actual SPSC pops here. */
    return 0;
}
