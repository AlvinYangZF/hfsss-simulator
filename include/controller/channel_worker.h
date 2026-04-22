#ifndef __HFSSS_CHANNEL_WORKER_H
#define __HFSSS_CHANNEL_WORKER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "common/common.h"
#include "common/spsc_ring.h"

struct media_ctx;
struct channel_cmd;

/*
 * REQ-044 per-channel worker runtime (REQ-045 scaffolding for async
 * completion; see REQUIREMENT_COVERAGE notes for the gap to a full
 * lock-free completion queue).
 *
 * A channel_worker owns one pthread pinned to a single logical channel
 * id and a single-producer single-consumer ring of channel_cmd
 * pointers. The producer side is any thread that calls
 * channel_worker_submit; the consumer is the worker thread which
 * dequeues commands, validates the target channel, dispatches them
 * through the synchronous media_nand_* API, publishes the done flag,
 * and finally fires the caller-supplied completion callback.
 *
 * This runtime is additive: it does not replace or retarget the
 * existing synchronous media call path. Callers that want async
 * submission opt in by constructing a channel_cmd and handing it to
 * submit. Existing sync callers are untouched.
 *
 * Thread-safety contract:
 *   - Exactly one thread may call channel_worker_submit per worker at a
 *     time. If multiple producers are required, the caller wraps submit
 *     with its own mutex. This matches the SPSC ring constraint.
 *   - The worker thread is the only consumer. cleanup() joins it.
 *   - Submits are rejected (HFSSS_ERR_BUSY) once stop has begun so late
 *     producers do not land commands on a shutting-down ring.
 *   - Commands addressed to a channel other than the worker's own
 *     channel_id are failed at dispatch with HFSSS_ERR_INVAL; they
 *     still complete via the normal callback/done path.
 *
 * Completion model — callers pick ONE of these patterns, not both:
 *   - Poll with channel_cmd_wait: the waiter blocks until done == 1
 *     and reads status. Waiter owns reclamation of the cmd afterwards.
 *   - Install on_complete: the callback fires AFTER done is published
 *     and is the authoritative reclamation hook; the waiter path must
 *     not run concurrently with a reclaiming callback on the same cmd.
 *
 * Ordering: commands are dispatched in submit order because the SPSC
 * ring is FIFO. In-flight media ops can themselves be reordered by the
 * NAND engine's EAT model, so FIFO at the worker boundary does not
 * imply FIFO at media-completion time.
 */

enum channel_cmd_op {
    CHANNEL_CMD_READ = 0,
    CHANNEL_CMD_PROGRAM,
    CHANNEL_CMD_ERASE,
};

typedef void (*channel_cmd_complete_fn)(struct channel_cmd *cmd, void *ctx);

struct channel_cmd {
    enum channel_cmd_op op;
    u32 ch;
    u32 chip;
    u32 die;
    u32 plane;
    u32 block;
    u32 page;
    /* READ: data_buf receives N bytes (page_size). PROGRAM: data_buf is
     * the source. ERASE: data_buf ignored. spare_buf is optional for
     * READ/PROGRAM paths that carry PI/OOB. */
    void *data_buf;
    void *spare_buf;

    /* Completion hook. Invoked exactly once by the worker thread after
     * the underlying media op returns, before done is published. NULL is
     * legal; pollers rely on the done flag instead. */
    channel_cmd_complete_fn on_complete;
    void *cb_ctx;

    /* Set by the worker thread. status is the int return from the
     * media_nand_* call; done transitions 0 -> 1 atomically and is
     * published with release ordering so a poller on another thread
     * can safely read status after seeing done == 1. */
    int status;
    _Atomic int done;
};

struct channel_worker {
    u32 channel_id;
    struct media_ctx *media;

    struct spsc_ring ring;       /* of struct channel_cmd * */
    pthread_t thread;
    bool thread_started;
    _Atomic int stop;

    /* Monotonic lifetime counters. submitted is advanced by the
     * producer after each successful tryput; completed is advanced by
     * the worker after each dispatch (regardless of media success or
     * failure). They are published independently so their delta is NOT
     * a reliable instantaneous in-flight depth — a fast consumer can
     * complete before the producer's submitted store lands, which
     * momentarily makes submitted - completed underflow. Use the
     * counters for aggregate rate monitoring, not for gauging
     * saturation. */
    _Atomic u64 submitted;
    _Atomic u64 completed;
};

/*
 * Initialize a channel_worker. ring_capacity must be a power of two.
 * media must outlive the worker. Returns HFSSS_OK on success. On
 * failure the worker is left in an unusable state and cleanup is a
 * no-op.
 */
int channel_worker_init(struct channel_worker *w, u32 channel_id, struct media_ctx *media, u32 ring_capacity);

/*
 * Signal the worker to stop, join the thread, and release ring memory.
 * Any channel_cmd still in the ring at stop time is discarded without
 * dispatch; callers must drain before cleanup if that matters. Safe to
 * call on a zero-initialized or failed-init struct.
 */
void channel_worker_cleanup(struct channel_worker *w);

/*
 * Signal stop without joining. The worker may still be draining the
 * ring when this returns. Use channel_worker_cleanup to block until
 * the thread is gone.
 */
void channel_worker_stop(struct channel_worker *w);

/*
 * Submit one command. Non-blocking: returns HFSSS_ERR_BUSY when the
 * ring is full. The caller retains ownership of the channel_cmd
 * storage and must keep it addressable until completion.
 */
int channel_worker_submit(struct channel_worker *w, struct channel_cmd *cmd);

/*
 * Block the caller until the given command's done flag is set. Spin
 * with sched_yield; a condvar variant can replace this if a workload
 * demonstrates producer-side idle cost. Returns cmd->status after
 * completion.
 */
int channel_cmd_wait(struct channel_cmd *cmd);

#endif /* __HFSSS_CHANNEL_WORKER_H */
