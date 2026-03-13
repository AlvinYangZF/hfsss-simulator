#include "controller/arbiter.h"
#include <stdlib.h>
#include <string.h>

/* Internal helper functions */
static void cmd_list_add_head(struct cmd_context **list, struct cmd_context *cmd);
static void cmd_list_add_tail(struct cmd_context **list, struct cmd_context *cmd);
static void cmd_list_remove(struct cmd_context **list, struct cmd_context *cmd);

static void cmd_list_add_head(struct cmd_context **list, struct cmd_context *cmd)
{
    if (!list || !cmd) {
        return;
    }

    cmd->next = *list;
    cmd->prev = NULL;

    if (*list) {
        (*list)->prev = cmd;
    }

    *list = cmd;
}

static void cmd_list_add_tail(struct cmd_context **list, struct cmd_context *cmd)
{
    if (!list || !cmd) {
        return;
    }

    if (!*list) {
        *list = cmd;
        cmd->next = NULL;
        cmd->prev = NULL;
        return;
    }

    struct cmd_context *tail = *list;
    while (tail->next) {
        tail = tail->next;
    }

    tail->next = cmd;
    cmd->prev = tail;
    cmd->next = NULL;
}

static void cmd_list_remove(struct cmd_context **list, struct cmd_context *cmd)
{
    if (!list || !cmd) {
        return;
    }

    if (cmd->prev) {
        cmd->prev->next = cmd->next;
    } else {
        /* Command was head of list */
        *list = cmd->next;
    }

    if (cmd->next) {
        cmd->next->prev = cmd->prev;
    }

    cmd->next = NULL;
    cmd->prev = NULL;
}

int arbiter_init(struct arbiter_ctx *ctx, u32 max_cmds)
{
    u32 i;
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    ctx->max_cmds = max_cmds;

    ctx->cmd_pool = (struct cmd_context *)calloc(max_cmds, sizeof(struct cmd_context));
    if (!ctx->cmd_pool) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    for (i = 0; i < max_cmds; i++) {
        ctx->cmd_pool[i].state = CMD_STATE_FREE;
        if (i < max_cmds - 1) {
            ctx->cmd_pool[i].next = &ctx->cmd_pool[i + 1];
        }
    }

    return HFSSS_OK;
}

void arbiter_cleanup(struct arbiter_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    free(ctx->cmd_pool);
    mutex_unlock(&ctx->lock);

    mutex_cleanup(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

struct cmd_context *arbiter_alloc_cmd(struct arbiter_ctx *ctx)
{
    struct cmd_context *cmd = NULL;
    u32 i;

    if (!ctx) {
        return NULL;
    }

    mutex_lock(&ctx->lock, 0);

    for (i = 0; i < ctx->max_cmds; i++) {
        if (ctx->cmd_pool[i].state == CMD_STATE_FREE) {
            cmd = &ctx->cmd_pool[i];
            memset(cmd, 0, sizeof(*cmd));
            cmd->state = CMD_STATE_RECEIVED;
            ctx->total_cmds++;
            break;
        }
    }

    mutex_unlock(&ctx->lock);

    return cmd;
}

void arbiter_free_cmd(struct arbiter_ctx *ctx, struct cmd_context *cmd)
{
    if (!ctx || !cmd) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    /* Remove from any queue */
    if (cmd->prev || cmd->next) {
        enum cmd_priority prio = cmd->priority;
        if (prio < HFSSS_PRIO_MAX) {
            cmd_list_remove(&ctx->queues[prio].head, cmd);
            ctx->queues[prio].count--;
        }
    }

    cmd->state = CMD_STATE_FREE;
    cmd->next = NULL;
    cmd->prev = NULL;
    ctx->total_cmds--;

    mutex_unlock(&ctx->lock);
}

int arbiter_enqueue(struct arbiter_ctx *ctx, struct cmd_context *cmd)
{
    if (!ctx || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    if (cmd->priority >= HFSSS_PRIO_MAX) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    cmd->state = CMD_STATE_ARBITRATED;
    cmd_list_add_tail(&ctx->queues[cmd->priority].head, cmd);
    ctx->queues[cmd->priority].count++;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

struct cmd_context *arbiter_dequeue(struct arbiter_ctx *ctx)
{
    struct cmd_context *cmd = NULL;
    int i;

    if (!ctx) {
        return NULL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Check priorities from highest to lowest */
    for (i = 0; i < HFSSS_PRIO_MAX; i++) {
        if (ctx->queues[i].head) {
            cmd = ctx->queues[i].head;
            cmd_list_remove(&ctx->queues[i].head, cmd);
            ctx->queues[i].count--;
            cmd->state = CMD_STATE_SCHEDULED;
            break;
        }
    }

    mutex_unlock(&ctx->lock);

    return cmd;
}
