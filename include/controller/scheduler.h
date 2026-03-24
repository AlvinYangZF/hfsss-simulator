#ifndef __HFSSS_SCHEDULER_H
#define __HFSSS_SCHEDULER_H

#include "common/common.h"
#include "common/mutex.h"
#include "controller/arbiter.h"

/* Scheduling Policy */
enum sched_policy {
    SCHED_FIFO = 0,
    SCHED_GREEDY = 1,
    SCHED_DEADLINE = 2,
    SCHED_WRR = 3,
    SCHED_DWRR = 4,
};

/* FIFO Scheduler */
struct sched_fifo {
    struct cmd_context *head;
    struct cmd_context *tail;
    u32 count;
};

/* Greedy Scheduler (LBA ordered) */
struct sched_greedy {
    struct cmd_context *tree_root;
    u32 count;
};

/* Deadline Scheduler */
struct sched_deadline {
    struct cmd_context *read_queue;
    struct cmd_context *write_queue;
    u32 read_count;
    u32 write_count;
    u32 read_batch;
    u32 write_batch;
};

/* Forward declaration for DWRR scheduler (defined in controller/qos.h) */
struct dwrr_scheduler;

/* Scheduler Context */
struct scheduler_ctx {
    enum sched_policy policy;
    union {
        struct sched_fifo fifo;
        struct sched_greedy greedy;
        struct sched_deadline deadline;
    } u;
    struct dwrr_scheduler *dwrr;  /* external DWRR scheduler (SCHED_DWRR) */
    u64 last_sched_ts;
    u64 sched_period_ns;
    struct mutex lock;
};

/* Function Prototypes */
int scheduler_init(struct scheduler_ctx *ctx, enum sched_policy policy);
void scheduler_cleanup(struct scheduler_ctx *ctx);
int scheduler_enqueue(struct scheduler_ctx *ctx, struct cmd_context *cmd);
struct cmd_context *scheduler_dequeue(struct scheduler_ctx *ctx);
int scheduler_set_policy(struct scheduler_ctx *ctx, enum sched_policy policy);

#endif /* __HFSSS_SCHEDULER_H */
