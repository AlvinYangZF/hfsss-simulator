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
    ctx->cmd_timeout_ns = (u64)DEFAULT_CMD_TIMEOUT_MS * 1000000ULL;

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

    /*
     * REQ-134: detach any fault registry before tearing down so no
     * in-flight path dereferences a soon-to-be-freed registry. The
     * attach/detach slot is a simple pointer so the store alone is
     * enough under the lock; mutators already serialize on ctx->lock.
     */
    mutex_lock(&ctx->lock, 0);
    ctx->faults = NULL;
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

    /*
     * REQ-134 pool-exhaustion hook also applies here: the arbiter's
     * cmd_pool is a bounded resource, so FAULT_POOL_EXHAUST gates
     * allocation just like resource_alloc / idle_block_alloc.
     * Checked under ctx->lock because ctx->faults is read under the
     * same lock in the other mutators.
     */
    mutex_lock(&ctx->lock, 0);

    if (ctx->faults) {
        struct fault_addr faddr = {
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
        };
        if (fault_check(ctx->faults, FAULT_POOL_EXHAUST, &faddr)) {
            mutex_unlock(&ctx->lock);
            return NULL;
        }
    }

    for (i = 0; i < ctx->max_cmds; i++) {
        if (ctx->cmd_pool[i].state == CMD_STATE_FREE) {
            cmd = &ctx->cmd_pool[i];
            memset(cmd, 0, sizeof(*cmd));
            cmd->state = CMD_STATE_RECEIVED;
            cmd->timestamp = get_time_ns();
            cmd->deadline = cmd->timestamp + ctx->cmd_timeout_ns;
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
        } else {
            /* Check in-flight queue */
            cmd_list_remove(&ctx->in_flight_queue.head, cmd);
            ctx->in_flight_queue.count--;
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

    /*
     * REQ-134: soft-panic injection. When FAULT_PANIC is armed the
     * enqueue is rejected and the command is left in CMD_STATE_ERROR
     * so the caller can observe the injected failure without the
     * process terminating. This is not a true controller panic
     * (cf. fault_controller_panic() which calls abort()); it is an
     * injectable surrogate that exercises the same error-return
     * paths real clients will take when the controller fails an
     * enqueue.
     *
     * The state write must happen under ctx->lock to race-protect
     * against a concurrent arbiter_mark_completed() / check_timeouts()
     * that also mutates cmd->state.
     */
    if (ctx->faults) {
        struct fault_addr faddr = {
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
        };
        if (fault_check(ctx->faults, FAULT_PANIC, &faddr)) {
            cmd->state = CMD_STATE_ERROR;
            mutex_unlock(&ctx->lock);
            return HFSSS_ERR;
        }
    }

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

/* Command Timeout Functions */
void arbiter_set_timeout(struct arbiter_ctx *ctx, u32 timeout_ms)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->cmd_timeout_ns = (u64)timeout_ms * 1000000ULL;
    mutex_unlock(&ctx->lock);
}

void arbiter_mark_in_flight(struct arbiter_ctx *ctx, struct cmd_context *cmd)
{
    if (!ctx || !cmd) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    cmd->state = CMD_STATE_IN_FLIGHT;
    cmd->in_flight_ts = get_time_ns();
    cmd_list_add_tail(&ctx->in_flight_queue.head, cmd);
    ctx->in_flight_queue.count++;

    mutex_unlock(&ctx->lock);
}

void arbiter_mark_completed(struct arbiter_ctx *ctx, struct cmd_context *cmd)
{
    if (!ctx || !cmd) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    /* Remove from in-flight queue */
    cmd_list_remove(&ctx->in_flight_queue.head, cmd);
    ctx->in_flight_queue.count--;

    if (cmd->state != CMD_STATE_TIMEOUT) {
        cmd->state = CMD_STATE_COMPLETED;
    }

    mutex_unlock(&ctx->lock);
}

u32 arbiter_check_timeouts(struct arbiter_ctx *ctx)
{
    u32 timeout_count = 0;
    u64 now;
    struct cmd_context *cmd, *next;
    bool force_all = false;

    if (!ctx) {
        return 0;
    }

    now = get_time_ns();

    mutex_lock(&ctx->lock, 0);

    /*
     * REQ-134: mass-timeout-detection injection. When FAULT_TIMEOUT
     * is armed, every in-flight command is forced through the
     * timeout path on this tick regardless of its deadline. Models a
     * controller stall that causes the host to observe mass
     * command-timeout at once. Under a sticky fault every subsequent
     * tick fires the same slam, so the window is closer to "stuck
     * in a timeout loop" than "a storm that clears"; one-shot or
     * probability-gated faults model a transient event.
     *
     * ctx->faults is read under ctx->lock to pair with
     * arbiter_attach_faults / arbiter_cleanup which store to it
     * under the same lock, avoiding a torn / use-after-free deref.
     */
    if (ctx->faults) {
        struct fault_addr faddr = {
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
            FAULT_WILDCARD, FAULT_WILDCARD, FAULT_WILDCARD,
        };
        if (fault_check(ctx->faults, FAULT_TIMEOUT, &faddr)) {
            force_all = true;
        }
    }

    cmd = ctx->in_flight_queue.head;
    while (cmd) {
        next = cmd->next;
        if (force_all || now > cmd->deadline) {
            /* Command timed out */
            cmd->state = CMD_STATE_TIMEOUT;
            ctx->stats.total_timeouts++;
            if (cmd->priority == PRIO_ADMIN_HIGH) {
                ctx->stats.admin_timeouts++;
            } else {
                ctx->stats.io_timeouts++;
            }
            ctx->stats.last_timeout_ts = now;
            timeout_count++;
        }
        cmd = next;
    }

    mutex_unlock(&ctx->lock);

    return timeout_count;
}

struct cmd_context *arbiter_get_next_timeout(struct arbiter_ctx *ctx)
{
    struct cmd_context *cmd = NULL;
    struct cmd_context *oldest = NULL;
    u64 oldest_deadline = U64_MAX;
    u64 now;

    if (!ctx) {
        return NULL;
    }

    now = get_time_ns();

    mutex_lock(&ctx->lock, 0);

    cmd = ctx->in_flight_queue.head;
    while (cmd) {
        if (cmd->state == CMD_STATE_IN_FLIGHT && cmd->deadline < oldest_deadline) {
            oldest_deadline = cmd->deadline;
            oldest = cmd;
        }
        cmd = cmd->next;
    }

    mutex_unlock(&ctx->lock);

    return oldest;
}

void arbiter_attach_faults(struct arbiter_ctx *ctx,
                           struct fault_registry *faults)
{
    if (!ctx) {
        return;
    }
    /*
     * Store under ctx->lock so the pointer swap is never observed
     * mid-write by a concurrent arbiter_enqueue / alloc_cmd /
     * check_timeouts. Matches the read-under-lock discipline in
     * those callers.
     */
    mutex_lock(&ctx->lock, 0);
    ctx->faults = faults;
    mutex_unlock(&ctx->lock);
}
