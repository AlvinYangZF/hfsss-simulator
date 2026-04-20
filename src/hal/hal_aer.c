/*
 * NVMe Asynchronous Event Request framework (REQ-063).
 *
 * Two ring queues behind a single mutex:
 *   - outstanding_cids: AER command identifiers awaiting completion
 *   - pending_events: events that fired before any AER was waiting
 *
 * All ops are O(1). Pointer to lockless hot-path exit omitted —
 * AER traffic is low-rate by spec and tests require strict ordering.
 */

#include "hal/hal_aer.h"
#include <string.h>

u32 hal_aer_dw0_encode(enum nvme_async_event_type type,
                       enum nvme_async_event_info info,
                       u8 log_page_id)
{
    u32 dw0 = 0;
    dw0 |= ((u32)type & 0x7);
    dw0 |= ((u32)info & 0xFF) << 8;
    dw0 |= ((u32)log_page_id & 0xFF) << 16;
    return dw0;
}

int hal_aer_init(struct hal_aer_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }
    memset(ctx, 0, sizeof(*ctx));
    int rc = pthread_mutex_init(&ctx->lock, NULL);
    if (rc != 0) {
        return HFSSS_ERR_IO;
    }
    ctx->initialized = true;
    return HFSSS_OK;
}

void hal_aer_cleanup(struct hal_aer_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return;
    }
    pthread_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

/* Pop the oldest pending event (caller holds ctx->lock). */
static struct nvme_aer_pending pop_pending_locked(struct hal_aer_ctx *ctx)
{
    struct nvme_aer_pending ev = ctx->pending[ctx->pending_head];
    ctx->pending_head = (ctx->pending_head + 1) % AER_PENDING_MAX;
    ctx->pending_count--;
    return ev;
}

/* Push an event to the pending ring; drops oldest on overflow so the
 * newest status always wins (NVMe spec leaves overflow behavior
 * implementation-defined; aging out stale events matches most SSDs). */
static void push_pending_locked(struct hal_aer_ctx *ctx,
                                const struct nvme_aer_pending *ev)
{
    if (ctx->pending_count == AER_PENDING_MAX) {
        ctx->pending_head = (ctx->pending_head + 1) % AER_PENDING_MAX;
        ctx->pending_count--;
    }
    u32 slot = (ctx->pending_head + ctx->pending_count) % AER_PENDING_MAX;
    ctx->pending[slot] = *ev;
    ctx->pending_count++;
}

static u16 pop_outstanding_locked(struct hal_aer_ctx *ctx)
{
    u16 cid = ctx->outstanding_cids[ctx->outstanding_head];
    ctx->outstanding_head = (ctx->outstanding_head + 1) % AER_REQUEST_MAX;
    ctx->outstanding_count--;
    return cid;
}

static void push_outstanding_locked(struct hal_aer_ctx *ctx, u16 cid)
{
    u32 slot = (ctx->outstanding_head + ctx->outstanding_count)
               % AER_REQUEST_MAX;
    ctx->outstanding_cids[slot] = cid;
    ctx->outstanding_count++;
}

static void build_completion(struct nvme_aer_completion *out, u16 cid,
                             enum nvme_async_event_type type,
                             enum nvme_async_event_info info,
                             u8 log_page_id,
                             u16 status_field)
{
    memset(out, 0, sizeof(*out));
    out->cid = cid;
    out->cqe.cdw0 = hal_aer_dw0_encode(type, info, log_page_id);
    out->cqe.command_id = cid;
    out->cqe.status = status_field;
}

int hal_aer_submit_request(struct hal_aer_ctx *ctx, u16 cid,
                           bool *was_immediate,
                           struct nvme_aer_completion *completed)
{
    if (!ctx || !ctx->initialized || !was_immediate || !completed) {
        return HFSSS_ERR_INVAL;
    }

    int rc = HFSSS_OK;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->pending_count > 0) {
        /* Event already buffered — complete immediately. */
        struct nvme_aer_pending ev = pop_pending_locked(ctx);
        build_completion(completed, cid, ev.type, ev.info,
                         ev.log_page_id,
                         NVME_BUILD_STATUS(NVME_SC_SUCCESS,
                                           NVME_STATUS_TYPE_GENERIC));
        *was_immediate = true;
    } else if (ctx->outstanding_count >= AER_REQUEST_MAX) {
        rc = HFSSS_ERR_NOSPC;
        *was_immediate = false;
        memset(completed, 0, sizeof(*completed));
    } else {
        push_outstanding_locked(ctx, cid);
        *was_immediate = false;
        memset(completed, 0, sizeof(*completed));
    }
    pthread_mutex_unlock(&ctx->lock);
    return rc;
}

int hal_aer_post_event(struct hal_aer_ctx *ctx,
                       enum nvme_async_event_type type,
                       enum nvme_async_event_info info,
                       u8 log_page_id,
                       bool *was_delivered,
                       struct nvme_aer_completion *completed)
{
    if (!ctx || !ctx->initialized || !was_delivered || !completed) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->outstanding_count > 0) {
        u16 cid = pop_outstanding_locked(ctx);
        build_completion(completed, cid, type, info, log_page_id,
                         NVME_BUILD_STATUS(NVME_SC_SUCCESS,
                                           NVME_STATUS_TYPE_GENERIC));
        *was_delivered = true;
    } else {
        struct nvme_aer_pending ev = {
            .type = type,
            .info = info,
            .log_page_id = log_page_id,
        };
        push_pending_locked(ctx, &ev);
        *was_delivered = false;
        memset(completed, 0, sizeof(*completed));
    }
    pthread_mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

u32 hal_aer_abort_pending(struct hal_aer_ctx *ctx,
                          struct nvme_aer_completion *out, u32 cap)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    pthread_mutex_lock(&ctx->lock);
    u32 total = ctx->outstanding_count;
    u32 emit = (cap == 0 || !out) ? 0
              : (total < cap ? total : cap);

    for (u32 i = 0; i < emit; i++) {
        u16 cid = pop_outstanding_locked(ctx);
        build_completion(&out[i], cid,
                         NVME_AER_TYPE_ERROR,
                         NVME_AEI_SMART_NVM_SUBSYS_RELIABILITY,
                         0,
                         NVME_BUILD_STATUS(NVME_SC_CMD_ABORT_REQUESTED,
                                           NVME_STATUS_TYPE_GENERIC));
    }
    /* If caller didn't pass storage for the rest, drop them with no
     * completion (host reset implies they'll be re-submitted). */
    while (ctx->outstanding_count > 0) {
        (void)pop_outstanding_locked(ctx);
    }
    pthread_mutex_unlock(&ctx->lock);
    return total;
}

u32 hal_aer_outstanding_count(struct hal_aer_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return 0;
    pthread_mutex_lock(&ctx->lock);
    u32 n = ctx->outstanding_count;
    pthread_mutex_unlock(&ctx->lock);
    return n;
}

u32 hal_aer_pending_count(struct hal_aer_ctx *ctx)
{
    if (!ctx || !ctx->initialized) return 0;
    pthread_mutex_lock(&ctx->lock);
    u32 n = ctx->pending_count;
    pthread_mutex_unlock(&ctx->lock);
    return n;
}
