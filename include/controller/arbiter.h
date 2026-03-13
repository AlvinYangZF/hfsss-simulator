#ifndef __HFSSS_ARBITER_H
#define __HFSSS_ARBITER_H

#include "common/common.h"
#include "common/mutex.h"
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
};

/* Command Context */
struct cmd_context {
    u64 cmd_id;
    enum cmd_type type;
    enum cmd_priority priority;
    enum cmd_state state;
    u64 timestamp;
    u64 deadline;
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

/* Arbiter Context */
struct arbiter_ctx {
    struct priority_queue queues[HFSSS_PRIO_MAX];
    u32 total_cmds;
    u32 max_cmds;
    struct cmd_context *cmd_pool;
    u32 pool_size;
    struct mutex lock;
};

/* Function Prototypes */
int arbiter_init(struct arbiter_ctx *ctx, u32 max_cmds);
void arbiter_cleanup(struct arbiter_ctx *ctx);
struct cmd_context *arbiter_alloc_cmd(struct arbiter_ctx *ctx);
void arbiter_free_cmd(struct arbiter_ctx *ctx, struct cmd_context *cmd);
int arbiter_enqueue(struct arbiter_ctx *ctx, struct cmd_context *cmd);
struct cmd_context *arbiter_dequeue(struct arbiter_ctx *ctx);

#endif /* __HFSSS_ARBITER_H */
