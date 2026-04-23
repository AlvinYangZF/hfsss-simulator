#ifndef __HFSSS_ARBITER_H
#define __HFSSS_ARBITER_H

#include "common/common.h"
#include "common/mutex.h"
#include "common/fault_inject.h"
#include "controller/shmem_if.h"

/* Command Priority */
enum cmd_priority {
    PRIO_ADMIN_HIGH = 0,
    PRIO_IO_URGENT = 1,
    PRIO_IO_HIGH = 2,
    PRIO_IO_NORMAL = 3,
    PRIO_IO_LOW = 4,
    HFSSS_PRIO_MAX = 5,
};

/* Command State */
enum cmd_state {
    CMD_STATE_FREE = 0,
    CMD_STATE_RECEIVED = 1,
    CMD_STATE_ARBITRATED = 2,
    CMD_STATE_SCHEDULED = 3,
    CMD_STATE_IN_FLIGHT = 4,
    CMD_STATE_COMPLETED = 5,
    CMD_STATE_ERROR = 6,
    CMD_STATE_TIMEOUT = 7,
};

/* Timeout Configuration */
#define DEFAULT_CMD_TIMEOUT_MS 30000  /* 30 seconds default timeout */

/* Command Context */
struct cmd_context {
    u64 cmd_id;
    enum cmd_type type;
    enum cmd_priority priority;
    enum cmd_state state;
    u64 timestamp;          /* Time when command was received */
    u64 deadline;           /* Time when command should complete */
    u64 in_flight_ts;       /* Time when command was marked in-flight */
    struct nvme_cmd_from_kern kern_cmd;
    void *user_data;
    struct cmd_context *next;
    struct cmd_context *prev;
};

/* Priority Queue */
struct priority_queue {
    struct cmd_context *head;
    struct cmd_context *tail;
    u32 count;
};

/* Timeout Statistics */
struct timeout_stats {
    u64 total_timeouts;
    u64 admin_timeouts;
    u64 io_timeouts;
    u64 last_timeout_ts;
};

/* Arbiter Context */
struct arbiter_ctx {
    struct priority_queue queues[HFSSS_PRIO_MAX];
    struct priority_queue in_flight_queue;  /* Commands currently in flight */
    u32 total_cmds;
    u32 max_cmds;
    struct cmd_context *cmd_pool;
    u32 pool_size;
    u64 cmd_timeout_ns;
    struct timeout_stats stats;
    struct mutex lock;

    /*
     * Optional fault-injection hook (REQ-134).
     *   FAULT_PANIC on arbiter_enqueue  -> enqueue rejected with HFSSS_ERR
     *   FAULT_TIMEOUT on check_timeouts -> every in-flight cmd forced to
     *                                      CMD_STATE_TIMEOUT this tick
     * NULL leaves the hot path untouched. Owned by the caller.
     */
    struct fault_registry *faults;
};

/* Function Prototypes */
int arbiter_init(struct arbiter_ctx *ctx, u32 max_cmds);
void arbiter_cleanup(struct arbiter_ctx *ctx);
struct cmd_context *arbiter_alloc_cmd(struct arbiter_ctx *ctx);
void arbiter_free_cmd(struct arbiter_ctx *ctx, struct cmd_context *cmd);
int arbiter_enqueue(struct arbiter_ctx *ctx, struct cmd_context *cmd);
struct cmd_context *arbiter_dequeue(struct arbiter_ctx *ctx);

/* Command Timeout Functions */
void arbiter_set_timeout(struct arbiter_ctx *ctx, u32 timeout_ms);
void arbiter_mark_in_flight(struct arbiter_ctx *ctx, struct cmd_context *cmd);
void arbiter_mark_completed(struct arbiter_ctx *ctx, struct cmd_context *cmd);
u32 arbiter_check_timeouts(struct arbiter_ctx *ctx);
struct cmd_context *arbiter_get_next_timeout(struct arbiter_ctx *ctx);

/* Fault injection attachment (REQ-134). Pass NULL to detach. */
void arbiter_attach_faults(struct arbiter_ctx *ctx,
                           struct fault_registry *faults);

#endif /* __HFSSS_ARBITER_H */
