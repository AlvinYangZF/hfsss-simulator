#include "controller/channel_worker.h"

#include <sched.h>
#include <string.h>

#include "media/media.h"

static int dispatch_one(struct channel_worker *w, struct channel_cmd *cmd)
{
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

static void *channel_worker_loop(void *arg)
{
    struct channel_worker *w = (struct channel_worker *)arg;

    for (;;) {
        if (atomic_load_explicit(&w->stop, memory_order_acquire)) {
            break;
        }

        struct channel_cmd *cmd = NULL;
        int rc = spsc_ring_tryget(&w->ring, &cmd);
        if (rc != HFSSS_OK || cmd == NULL) {
            sched_yield();
            continue;
        }

        cmd->status = dispatch_one(w, cmd);
        if (cmd->on_complete) {
            cmd->on_complete(cmd, cmd->cb_ctx);
        }
        /* Publish done AFTER on_complete so a waiter that wakes on done
         * observes the effects of the completion callback. Release
         * ordering pairs with acquire in channel_cmd_wait. */
        atomic_store_explicit(&cmd->done, 1, memory_order_release);
        atomic_fetch_add_explicit(&w->completed, 1, memory_order_relaxed);
    }

    return NULL;
}

int channel_worker_init(struct channel_worker *w, u32 channel_id, struct media_ctx *media, u32 ring_capacity)
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

    if (pthread_create(&w->thread, NULL, channel_worker_loop, w) != 0) {
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
    spsc_ring_cleanup(&w->ring);
    memset(w, 0, sizeof(*w));
}

int channel_worker_submit(struct channel_worker *w, struct channel_cmd *cmd)
{
    if (!w || !cmd) {
        return HFSSS_ERR_INVAL;
    }
    atomic_store_explicit(&cmd->done, 0, memory_order_relaxed);
    cmd->status = 0;

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
    while (atomic_load_explicit(&cmd->done, memory_order_acquire) == 0) {
        sched_yield();
    }
    return cmd->status;
}
