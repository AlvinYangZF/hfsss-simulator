#include "controller/scheduler.h"
#include "controller/qos.h"
#include <stdlib.h>
#include <string.h>

int scheduler_init(struct scheduler_ctx *ctx, enum sched_policy policy)
{
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ctx->policy = policy;
    ctx->sched_period_ns = 10000;  /* 10us default */

    return HFSSS_OK;
}

void scheduler_cleanup(struct scheduler_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    mutex_unlock(&ctx->lock);

    mutex_cleanup(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

int scheduler_enqueue(struct scheduler_ctx *ctx, struct cmd_context *cmd)
{
    if (!ctx || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    switch (ctx->policy) {
    case SCHED_FIFO:
        if (ctx->u.fifo.head == NULL) {
            ctx->u.fifo.head = cmd;
            ctx->u.fifo.tail = cmd;
            cmd->next = NULL;
            cmd->prev = NULL;
        } else {
            ctx->u.fifo.tail->next = cmd;
            cmd->prev = ctx->u.fifo.tail;
            cmd->next = NULL;
            ctx->u.fifo.tail = cmd;
        }
        ctx->u.fifo.count++;
        break;

    case SCHED_GREEDY:
    case SCHED_DEADLINE:
    case SCHED_WRR:
        /* Fallback to FIFO for simplicity */
        if (ctx->u.fifo.head == NULL) {
            ctx->u.fifo.head = cmd;
            ctx->u.fifo.tail = cmd;
            cmd->next = NULL;
            cmd->prev = NULL;
        } else {
            ctx->u.fifo.tail->next = cmd;
            cmd->prev = ctx->u.fifo.tail;
            cmd->next = NULL;
            ctx->u.fifo.tail = cmd;
        }
        ctx->u.fifo.count++;
        break;

    case SCHED_DWRR:
        /* Route to DWRR scheduler.  Namespace ID is extracted from the
         * NVMe command CDW1 (kern_cmd.cdw[1]) when multi-NS is active;
         * default to nsid=1 for single-namespace configurations. */
        if (ctx->dwrr) {
            u32 nsid = 1;
            if (cmd->kern_cmd.cdw0_15[1] > 0) {
                nsid = cmd->kern_cmd.cdw0_15[1];
            }
            int dwrr_ret = dwrr_enqueue(ctx->dwrr, nsid);
            if (dwrr_ret != HFSSS_OK) {
                /* Fallback: enqueue to FIFO if DWRR rejects */
                if (ctx->u.fifo.head == NULL) {
                    ctx->u.fifo.head = cmd;
                    ctx->u.fifo.tail = cmd;
                    cmd->next = NULL;
                    cmd->prev = NULL;
                } else {
                    ctx->u.fifo.tail->next = cmd;
                    cmd->prev = ctx->u.fifo.tail;
                    cmd->next = NULL;
                    ctx->u.fifo.tail = cmd;
                }
                ctx->u.fifo.count++;
            }
        }
        break;

    default:
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_INVAL;
    }

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

struct cmd_context *scheduler_dequeue(struct scheduler_ctx *ctx)
{
    struct cmd_context *cmd = NULL;

    if (!ctx) {
        return NULL;
    }

    mutex_lock(&ctx->lock, 0);

    switch (ctx->policy) {
    case SCHED_FIFO:
    case SCHED_GREEDY:
    case SCHED_DEADLINE:
    case SCHED_WRR:
        if (ctx->u.fifo.head) {
            cmd = ctx->u.fifo.head;
            ctx->u.fifo.head = cmd->next;
            if (ctx->u.fifo.head) {
                ctx->u.fifo.head->prev = NULL;
            } else {
                ctx->u.fifo.tail = NULL;
            }
            cmd->next = NULL;
            cmd->prev = NULL;
            ctx->u.fifo.count--;
        }
        break;

    default:
        break;
    }

    mutex_unlock(&ctx->lock);

    return cmd;
}

int scheduler_set_policy(struct scheduler_ctx *ctx, enum sched_policy policy)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->policy = policy;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}
