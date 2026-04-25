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
 * Completion model — callers pick EXACTLY ONE of these patterns per
 * worker, decided at channel_worker_init time:
 *   - cq_capacity == 0  (default / back-compat):
 *       * Poll with channel_cmd_wait: the waiter blocks until done == 1
 *         and reads status. Waiter owns reclamation of the cmd afterwards.
 *       * Install on_complete: the callback fires AFTER done is published
 *         and is the authoritative reclamation hook; the waiter path must
 *         not run concurrently with a reclaiming callback on the same cmd.
 *   - cq_capacity >  0  (REQ-045 batch-drain mode):
 *       * channel_worker_drain pops completed cmd pointers from the
 *         dedicated completion queue. on_complete on submitted cmds is
 *         rejected at submit time; channel_cmd_wait is rejected at wait
 *         time. The worker spin-retries CQ pushes when the CQ fills, so
 *         no completion is ever dropped — back-pressure flows through to
 *         the submit ring instead.
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

struct channel_worker; /* forward; full def appears later in this header */

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
     * legal; pollers rely on the done flag instead. Must be NULL when
     * the owning worker has cq_capacity > 0; that path delivers via
     * channel_worker_drain instead. */
    channel_cmd_complete_fn on_complete;
    void *cb_ctx;

    /* Set by the worker thread. status is the int return from the
     * media_nand_* call; done transitions 0 -> 1 atomically and is
     * published with release ordering so a poller on another thread
     * can safely read status after seeing done == 1. */
    int status;
    _Atomic int done;

    /* REQ-045: wall-clock timestamps. submit_ts_ns is stamped by the
     * producer thread inside channel_worker_submit before the ring put.
     * complete_ts_ns is stamped by the worker thread immediately after
     * the underlying media_nand_* call returns and BEFORE done is
     * published. Both come from get_time_ns() (CLOCK_MONOTONIC).
     * Consumers compute actual_latency = complete_ts_ns - submit_ts_ns
     * and may safely read both fields after observing done == 1 with
     * acquire ordering — the release on done carries the prior stores.
     * Recorded unconditionally (cost: two clock_gettime calls per
     * submission), so callers using channel_cmd_wait or on_complete
     * see them too, not just CQ consumers. */
    u64 submit_ts_ns;
    u64 complete_ts_ns;

    /* REQ-045: back-pointer to the worker that owns this cmd. Set
     * exactly once by channel_worker_submit; read only by
     * channel_cmd_wait to enforce CQ-mode mutual exclusion. Never
     * dereferenced after completion; cmd lifetime does not depend on
     * worker lifetime once done is observed. */
    struct channel_worker *owner;
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

    /* REQ-045: opt-in completion queue. cq_enabled is set by
     * channel_worker_init when cq_capacity > 0; cq is the second
     * SPSC ring whose payload is struct channel_cmd *. When enabled,
     * the worker spin-retries cq tryput after each completion (it
     * never drops a completion); when disabled, both fields stay
     * zero-initialised and the runtime behaves as before. */
    bool             cq_enabled;
    struct spsc_ring cq;
};

/*
 * Initialize a channel_worker. ring_capacity must be a power of two.
 * media must outlive the worker. cq_capacity == 0 disables the
 * completion queue and preserves the existing wait/callback delivery
 * model. cq_capacity > 0 must be a power of two, allocates a second
 * SPSC ring as the dedicated completion queue, and switches delivery
 * to channel_worker_drain (in this mode, on_complete on submitted
 * cmds and channel_cmd_wait calls are rejected — see the three-path
 * mutual exclusion contract in the file header). Returns HFSSS_OK on
 * success. On failure the worker is left in an unusable state and
 * cleanup is a no-op.
 */
int channel_worker_init(struct channel_worker *w,
                        u32 channel_id,
                        struct media_ctx *media,
                        u32 ring_capacity,
                        u32 cq_capacity);

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

/*
 * REQ-045: non-blocking batch drain of the completion queue.
 *
 *   buf     — caller-provided array of struct channel_cmd * slots.
 *   buf_cap — number of slots in buf. Must be > 0.
 *
 * Returns the number of commands popped (0..buf_cap). Returns 0
 * immediately when the CQ is empty; never blocks or yields. Returns
 * HFSSS_ERR_INVAL when cq_enabled is false (CQ disabled at init) or
 * when buf is NULL or buf_cap == 0.
 *
 * Ordering: pops happen in worker-enqueue order (SPSC FIFO). Media-
 * layer reordering (per EAT) is upstream of the CQ, so this FIFO does
 * not imply submit-time FIFO.
 *
 * Safety: exactly one thread may call this at a time (SPSC consumer).
 * Command ownership/reclamation is the caller's responsibility, same
 * as on the wait/callback paths.
 */
int channel_worker_drain(struct channel_worker *w,
                         struct channel_cmd **buf,
                         u32 buf_cap);

#endif /* __HFSSS_CHANNEL_WORKER_H */
