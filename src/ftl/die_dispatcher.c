/*
 * FTL die-busy wait queue: data structure + lifecycle.
 *
 * Implements the per-die multi-tier wait queue described in
 * docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md.
 * The wait/notifier bodies live with the multi-thread integration; this
 * file owns the structural pieces (queue, list, WFQ, EMA, tier map).
 */

#define _GNU_SOURCE

#include "die_dispatcher_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Env-gated fault-injection knobs                                    */
/* ------------------------------------------------------------------ */

/*
 * Knobs are off by default. When the env var is unset, both helpers
 * return 0 and every fault-injection branch is a single relaxed-load
 * away from a no-op — release builds with neither env set are
 * byte-equivalent to the previous behavior.
 *
 * Pattern matches HFSSS_TRACE_IO_ERR (include/common/io_err_trace.h):
 * getenv on first probe, cache result in a relaxed-atomic, all
 * subsequent probes pay only one relaxed load.
 *
 * Sentinel for the unread state is -1 (signed cache) and (uint64_t)-1
 * (unsigned cache) so a legitimate zero env value caches as zero.
 */

static _Atomic int      g_force_busy_pct       = -1;
static _Atomic uint64_t g_notifier_delay_ns    = (uint64_t)-1;

/*
 * Returns the configured force-BUSY injection rate (0-100). When > 0,
 * a successful wake from die_dispatcher_wait may instead return
 * ETIMEDOUT with this percentage probability; the FTL retry loop
 * already treats ETIMEDOUT as "fall through to next iteration", so
 * this exercises the re-queue path without changing any caller.
 */
static int die_disp_force_busy_pct(void)
{
    int v = atomic_load_explicit(&g_force_busy_pct, memory_order_relaxed);
    if (v == -1) {
        const char *e = getenv("HFSSS_DIE_DISP_FORCE_BUSY");
        if (!e || !*e) {
            v = 0;
        } else {
            v = atoi(e);
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
        }
        atomic_store_explicit(&g_force_busy_pct, v, memory_order_relaxed);
    }
    return v;
}

/*
 * Returns the configured notifier-internal delay in nanoseconds. When
 * > 0, die_dispatcher_on_die_ready sleeps for this duration between
 * dequeueing a waiter (under q->lock) and signaling its cv. This
 * widens the wake -> resubmit window so spurious-wake guards in the
 * waiter and re-queue race conditions can be exercised.
 */
static uint64_t die_disp_notifier_delay_ns(void)
{
    uint64_t v =
        atomic_load_explicit(&g_notifier_delay_ns, memory_order_relaxed);
    if (v == (uint64_t)-1) {
        const char *e = getenv("HFSSS_DIE_DISP_NOTIFIER_DELAY_NS");
        v = (e && *e) ? (uint64_t)strtoull(e, NULL, 10) : 0;
        atomic_store_explicit(&g_notifier_delay_ns, v, memory_order_relaxed);
    }
    return v;
}

/*
 * Test-only helper. Resets both env caches so a test can install a
 * different env value with setenv() and have the next probe re-read it.
 * No-op in production; declared in die_dispatcher_internal.h for tests.
 */
void die_dispatcher_reset_env_cache_for_testing(void)
{
    atomic_store_explicit(&g_force_busy_pct, -1, memory_order_relaxed);
    atomic_store_explicit(&g_notifier_delay_ns, (uint64_t)-1,
                          memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Tier and slot mapping                                              */
/* ------------------------------------------------------------------ */

int die_priority_tier(die_priority_t prio)
{
    switch (prio) {
    case DIE_PRIO_GC_CRITICAL: return 0;  /* T1 */
    case DIE_PRIO_HOST_READ:   return 1;  /* T2, slot 0 */
    case DIE_PRIO_GC_FORCE:    return 1;  /* T2, slot 1 */
    case DIE_PRIO_HOST_WRITE:  return 2;  /* T3, slot 0 */
    case DIE_PRIO_GC_NORMAL:   return 2;  /* T3, slot 1 */
    case DIE_PRIO_GC_IDLE:     return 3;  /* T4 */
    default: return -1;
    }
}

int die_priority_slot(die_priority_t prio)
{
    switch (prio) {
    case DIE_PRIO_HOST_READ:   return 0;
    case DIE_PRIO_GC_FORCE:    return 1;
    case DIE_PRIO_HOST_WRITE:  return 0;
    case DIE_PRIO_GC_NORMAL:   return 1;
    default: return -1;
    }
}

die_priority_t die_dispatcher_prio_for_gc(gc_trigger_t t)
{
    switch (t) {
    case GC_TRIGGER_FREE_SB_LOW:   return DIE_PRIO_GC_CRITICAL;
    case GC_TRIGGER_READ_DISTURB:  return DIE_PRIO_GC_FORCE;
    case GC_TRIGGER_WEAR_LEVELING: return DIE_PRIO_GC_FORCE;
    case GC_TRIGGER_HOST_WRITE:    return DIE_PRIO_GC_NORMAL;
    case GC_TRIGGER_IDLE:          return DIE_PRIO_GC_IDLE;
    }
    return DIE_PRIO_GC_NORMAL;
}

/* ------------------------------------------------------------------ */
/* Waiter / waitqueue init + cleanup                                  */
/* ------------------------------------------------------------------ */

void die_waiter_init(struct die_waiter *w, die_priority_t prio)
{
    dispatch_list_init(&w->list);
    pthread_cond_init(&w->cv, NULL);
    pthread_mutex_init(&w->cv_lock, NULL);
    w->signaled = false;
    w->shutdown = false;
    w->prio = prio;
}

void die_waiter_cleanup(struct die_waiter *w)
{
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->cv_lock);
}

void die_waitqueue_init(struct die_waitqueue *q)
{
    pthread_mutex_init(&q->lock, NULL);
    for (int i = 0; i < DIE_DISPATCH_TIER_COUNT; i++) {
        dispatch_list_init(&q->tier[i]);
    }
    for (int i = 0; i < DIE_DISPATCH_WFQ_SLOT_COUNT; i++) {
        q->wfq_deficit_t2[i] = 0;
        q->wfq_deficit_t3[i] = 0;
        q->wfq_quantum_t2[i] = DIE_DISPATCH_WFQ_DEFAULT_QUANTUM;
        q->wfq_quantum_t3[i] = DIE_DISPATCH_WFQ_DEFAULT_QUANTUM;
    }
    q->ema_iat_ns = 0.0;
    q->last_host_ts_ns = 0;
    q->ema_alpha = DIE_DISPATCH_EMA_DEFAULT_ALPHA;
    q->yield_threshold_ns = DIE_DISPATCH_DEFAULT_YIELD_NS;
    q->burst_grace_ns = DIE_DISPATCH_DEFAULT_GRACE_NS;
}

void die_waitqueue_cleanup(struct die_waitqueue *q)
{
    pthread_mutex_destroy(&q->lock);
}

/* ------------------------------------------------------------------ */
/* Enqueue / pop                                                      */
/* ------------------------------------------------------------------ */

void die_waitqueue_enqueue(struct die_waitqueue *q, struct die_waiter *w)
{
    int tier = die_priority_tier(w->prio);
    if (tier < 0) {
        return;
    }
    dispatch_list_add_tail(&q->tier[tier], &w->list);
}

/*
 * Walk one tier's list and return the head node belonging to `slot`. If no
 * node from that slot is present, returns NULL.
 */
static struct die_waiter *find_first_in_slot(struct dispatch_list *head,
                                             int slot)
{
    for (struct dispatch_list *cur = head->next; cur != head; cur = cur->next) {
        struct die_waiter *w = (struct die_waiter *)cur;
        if (die_priority_slot(w->prio) == slot) {
            return w;
        }
    }
    return NULL;
}

/*
 * Deficit-round-robin pick over a two-slot tier. Each slot's deficit counts
 * how much it has been served so far; the dispatcher picks the slot that
 * has been served the least (smaller deficit). On tie slot 0 wins.
 *
 * Increment the chosen slot's deficit by its quantum after picking; the
 * other slot's deficit stays put, and the next pass naturally picks it
 * because its deficit is now smaller. With equal quanta {1,1} this yields
 * exact alternation.
 */
static struct die_waiter *wfq_pick(struct dispatch_list *head,
                                   int32_t *deficit,
                                   const int32_t *quantum)
{
    struct die_waiter *cand[DIE_DISPATCH_WFQ_SLOT_COUNT];
    cand[0] = find_first_in_slot(head, 0);
    cand[1] = find_first_in_slot(head, 1);

    int chosen;
    if (cand[0] && cand[1]) {
        chosen = (deficit[0] <= deficit[1]) ? 0 : 1;
    } else if (cand[0]) {
        chosen = 0;
    } else if (cand[1]) {
        chosen = 1;
    } else {
        return NULL;
    }

    deficit[chosen] += quantum[chosen];
    dispatch_list_del(&cand[chosen]->list);
    return cand[chosen];
}

static struct die_waiter *pop_strict_head(struct dispatch_list *head)
{
    if (dispatch_list_empty(head)) {
        return NULL;
    }
    struct die_waiter *w = (struct die_waiter *)head->next;
    dispatch_list_del(&w->list);
    return w;
}

struct die_waiter *die_waitqueue_pop_next(struct die_waitqueue *q)
{
    /* T1 strict pre-emption. */
    if (!dispatch_list_empty(&q->tier[0])) {
        return pop_strict_head(&q->tier[0]);
    }
    /* T2 WFQ between HOST_READ and GC_FORCE. */
    if (!dispatch_list_empty(&q->tier[1])) {
        struct die_waiter *w =
            wfq_pick(&q->tier[1], q->wfq_deficit_t2, q->wfq_quantum_t2);
        if (w) {
            return w;
        }
    }
    /* T3 WFQ between HOST_WRITE and GC_NORMAL. */
    if (!dispatch_list_empty(&q->tier[2])) {
        struct die_waiter *w =
            wfq_pick(&q->tier[2], q->wfq_deficit_t3, q->wfq_quantum_t3);
        if (w) {
            return w;
        }
    }
    /* T4 strict-low; only fires when host is not bursting. */
    if (!dispatch_list_empty(&q->tier[3])) {
        u64 now = get_time_ns();
        if (!die_waitqueue_host_bursting(q, now)) {
            return pop_strict_head(&q->tier[3]);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Host pressure (EMA)                                                */
/* ------------------------------------------------------------------ */

/*
 * EMA update on every host-IO dispatch. The first call after a fresh queue
 * (or after a reset of last_host_ts_ns) only seeds last_host_ts; the second
 * call seeds ema to the observed IAT directly so the predicate gives a
 * sensible answer after just two samples instead of waiting for the
 * 1/alpha-iteration warm-up. Subsequent calls use the standard EMA recursion
 * `ema <- ema * (1 - alpha) + iat * alpha`.
 */
void die_waitqueue_record_host_io(struct die_waitqueue *q, u64 now_ns)
{
    if (q->last_host_ts_ns == 0) {
        q->last_host_ts_ns = now_ns;
        q->ema_iat_ns = 0.0;
        return;
    }
    double iat = (now_ns > q->last_host_ts_ns)
                     ? (double)(now_ns - q->last_host_ts_ns)
                     : 0.0;
    if (q->ema_iat_ns <= 0.0) {
        q->ema_iat_ns = iat;
    } else {
        q->ema_iat_ns =
            q->ema_iat_ns * (1.0 - q->ema_alpha) + iat * q->ema_alpha;
    }
    q->last_host_ts_ns = now_ns;
}

/*
 * Host pressure predicate. Returns true when the dispatcher should refuse to
 * schedule T4 (idle GC) waiters in favor of waiting for the next host IO.
 *
 * Semantics:
 *   - If no host IO has ever been recorded, host is not bursting.
 *   - If the last host IO is older than burst_grace_ns, host is no longer
 *     bursting regardless of how recently the EMA was high — this is the
 *     decay override that lets idle GC drain when the host genuinely backs
 *     off (EMA itself does not decay without fresh samples).
 *   - Otherwise, the host is bursting when either the EMA-tracked rate is
 *     above threshold (ema_iat_ns < yield_threshold_ns) or when the IO is
 *     recent enough that we have not yet built an EMA picture; the
 *     starvation guard keeps an idle GC out of the gap between two back-to-
 *     back host IOs.
 */
bool die_waitqueue_host_bursting(struct die_waitqueue *q, u64 now_ns)
{
    if (q->last_host_ts_ns == 0) {
        return false;
    }
    u64 age_ns = (now_ns > q->last_host_ts_ns)
                     ? (now_ns - q->last_host_ts_ns)
                     : 0;
    if (age_ns >= q->burst_grace_ns) {
        return false;
    }
    if (q->ema_iat_ns > 0.0 &&
        q->ema_iat_ns < (double)q->yield_threshold_ns) {
        return true;
    }
    /* Within the grace window with no useful EMA yet — treat as bursty so
     * idle GC stays out until the EMA stabilizes or the grace expires. */
    return true;
}

/* ------------------------------------------------------------------ */
/* Lifecycle (create / destroy)                                       */
/* ------------------------------------------------------------------ */

/*
 * Geometry resolution. The nand_device exposes per-channel chip_count and
 * per-chip die_count, but the dispatcher needs a flat (ch,chip,die) -> queue
 * index. We require the geometry to be uniform (every channel has the same
 * chips_per_channel; every chip has the same dies_per_chip). Real device
 * config in this project is always uniform, so we record the first
 * channel/chip's counts at create time and use them for indexing.
 */
static u32 dev_chips_per_channel(struct nand_device *dev)
{
    if (dev->channel_count == 0) {
        return 0;
    }
    return dev->channels[0].chip_count;
}

static u32 dev_dies_per_chip(struct nand_device *dev)
{
    if (dev->channel_count == 0 || dev->channels[0].chip_count == 0) {
        return 0;
    }
    return dev->channels[0].chips[0].die_count;
}

struct die_dispatcher *die_dispatcher_create(struct nand_device *dev)
{
    if (!dev) {
        return NULL;
    }
    struct die_dispatcher *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->dev = dev;
    d->chips_per_channel = dev_chips_per_channel(dev);
    d->dies_per_chip = dev_dies_per_chip(dev);
    d->queue_count = dev->channel_count * d->chips_per_channel * d->dies_per_chip;

    if (d->queue_count == 0) {
        /* No backing geometry; still install the hook so callers can
         * destroy cleanly. queues stays NULL. */
        dev->die_ready_notifier = die_dispatcher_on_die_ready;
        dev->die_ready_ctx = d;
        return d;
    }

    d->queues = calloc(d->queue_count, sizeof(*d->queues));
    if (!d->queues) {
        free(d);
        return NULL;
    }
    for (u32 i = 0; i < d->queue_count; i++) {
        die_waitqueue_init(&d->queues[i]);
    }
    dev->die_ready_notifier = die_dispatcher_on_die_ready;
    dev->die_ready_ctx = d;
    return d;
}

/*
 * Drain any pending waiters under shutdown semantics. Marks every queued
 * waiter with shutdown=true and wakes it; the caller of die_dispatcher_wait
 * observes the flag and returns HFSSS_ERR_SHUTDOWN. Each waiter's storage is
 * stack-allocated by the caller, so the caller must complete its wait (and
 * be joined by its parent) before the dispatcher's queue memory is freed —
 * this is documented as a precondition of die_dispatcher_destroy.
 */
static void die_waitqueue_drain_for_shutdown(struct die_waitqueue *q)
{
    pthread_mutex_lock(&q->lock);
    for (int t = 0; t < DIE_DISPATCH_TIER_COUNT; t++) {
        while (!dispatch_list_empty(&q->tier[t])) {
            struct die_waiter *w = (struct die_waiter *)q->tier[t].next;
            dispatch_list_del(&w->list);
            pthread_mutex_lock(&w->cv_lock);
            w->shutdown = true;
            w->signaled = true;
            pthread_cond_signal(&w->cv);
            pthread_mutex_unlock(&w->cv_lock);
        }
    }
    pthread_mutex_unlock(&q->lock);
}

void die_dispatcher_destroy(struct die_dispatcher *d)
{
    if (!d) {
        return;
    }
    /* Uninstall the notifier hook first so no fresh wake events arrive
     * after we begin tearing down queue state. */
    if (d->dev) {
        d->dev->die_ready_notifier = NULL;
        d->dev->die_ready_ctx = NULL;
    }
    if (d->queues) {
        for (u32 i = 0; i < d->queue_count; i++) {
            die_waitqueue_drain_for_shutdown(&d->queues[i]);
        }
        for (u32 i = 0; i < d->queue_count; i++) {
            die_waitqueue_cleanup(&d->queues[i]);
        }
        free(d->queues);
    }
    free(d);
}

/* ------------------------------------------------------------------ */
/* Wait / notifier                                                    */
/* ------------------------------------------------------------------ */

/*
 * Resolve a flat queue pointer for (ch, chip, die). Returns NULL when the
 * dispatcher has no queue array (e.g., an empty-geometry device) or when
 * the indices are out of range.
 */
static struct die_waitqueue *pick_queue(struct die_dispatcher *d,
                                        u32 ch, u32 chip, u32 die)
{
    if (!d || !d->queues) {
        return NULL;
    }
    if (d->chips_per_channel == 0 || d->dies_per_chip == 0) {
        return NULL;
    }
    u64 idx = (u64)ch * d->chips_per_channel * d->dies_per_chip
            + (u64)chip * d->dies_per_chip
            + die;
    if (idx >= d->queue_count) {
        return NULL;
    }
    return &d->queues[idx];
}

/*
 * Compute an absolute deadline `max_wait_ms` from now using CLOCK_REALTIME,
 * which is what pthread_cond_timedwait expects by default. Wraps the seconds
 * field so we don't overflow the nanoseconds slot.
 */
static void abs_deadline_ms(u32 max_wait_ms, struct timespec *out)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    u64 add_ns = (u64)max_wait_ms * 1000000ULL;
    u64 total_ns = (u64)now.tv_nsec + add_ns;
    out->tv_sec = now.tv_sec + (time_t)(total_ns / 1000000000ULL);
    out->tv_nsec = (long)(total_ns % 1000000000ULL);
}

int die_dispatcher_wait(struct die_dispatcher *d,
                        u32 ch, u32 chip, u32 die,
                        die_priority_t prio,
                        u32 max_wait_ms)
{
    if (!d) {
        return HFSSS_ERR_INVAL;
    }
    if (die_priority_tier(prio) < 0) {
        return HFSSS_ERR_INVAL;
    }
    struct die_waitqueue *q = pick_queue(d, ch, chip, die);
    if (!q) {
        return HFSSS_ERR_INVAL;
    }

    struct die_waiter w;
    die_waiter_init(&w, prio);

    /* Enqueue under q->lock — visible to any subsequent on_die_ready. */
    pthread_mutex_lock(&q->lock);
    die_waitqueue_enqueue(q, &w);
    pthread_mutex_unlock(&q->lock);

    /* Block on the per-waiter cv. The signaled / shutdown flags are set
     * under cv_lock by the wakers so spurious wakes are handled. */
    int wait_rc = 0;
    bool timed_out = false;
    pthread_mutex_lock(&w.cv_lock);
    while (!w.signaled && !w.shutdown) {
        if (max_wait_ms == 0) {
            pthread_cond_wait(&w.cv, &w.cv_lock);
        } else {
            struct timespec ts;
            abs_deadline_ms(max_wait_ms, &ts);
            wait_rc = pthread_cond_timedwait(&w.cv, &w.cv_lock, &ts);
            if (wait_rc == ETIMEDOUT) {
                timed_out = true;
                break;
            }
        }
    }
    bool was_shutdown = w.shutdown;
    pthread_mutex_unlock(&w.cv_lock);

    /*
     * On timeout, the waiter may still be linked into the queue (no notifier
     * dequeued it). Self-remove under q->lock; dispatch_list_del() left a
     * popped node self-looping, so an empty list head is the "already
     * dequeued" indicator.
     *
     * On shutdown, destroy already removed the waiter from the queue while
     * holding q->lock, so we must NOT touch q here — its memory may be on
     * its way out. Returning straight to the caller is safe because the
     * caller's stack frame still owns the cv/cv_lock.
     */
    int rc = 0;
    if (was_shutdown) {
        rc = HFSSS_ERR_SHUTDOWN;
    } else if (timed_out) {
        pthread_mutex_lock(&q->lock);
        if (!dispatch_list_empty(&w.list)) {
            dispatch_list_del(&w.list);
        }
        pthread_mutex_unlock(&q->lock);
        rc = ETIMEDOUT;
    } else {
        /*
         * Force-BUSY fault injection. On a real signaled wake, with
         * probability pct%, override rc to ETIMEDOUT so the caller's
         * retry loop re-submits and re-waits as if cmd_engine had
         * returned BUSY again. The waiter is already dequeued by the
         * notifier, so no queue cleanup is required. Default off.
         */
        int pct = die_disp_force_busy_pct();
        if (pct > 0 && (rand() % 100) < pct) {
            rc = ETIMEDOUT;
        }
    }

    die_waiter_cleanup(&w);
    return rc;
}

void die_dispatcher_on_die_ready(struct nand_device *dev,
                                 u32 ch, u32 chip, u32 die)
{
    if (!dev) {
        return;
    }
    struct die_dispatcher *d = dev->die_ready_ctx;
    if (!d) {
        return;
    }
    struct die_waitqueue *q = pick_queue(d, ch, chip, die);
    if (!q) {
        return;
    }

    struct die_waiter *to_wake;
    pthread_mutex_lock(&q->lock);
    to_wake = die_waitqueue_pop_next(q);
    if (to_wake &&
        (to_wake->prio == DIE_PRIO_HOST_READ ||
         to_wake->prio == DIE_PRIO_HOST_WRITE)) {
        die_waitqueue_record_host_io(q, get_time_ns());
    }
    pthread_mutex_unlock(&q->lock);

    if (!to_wake) {
        return;
    }
    /*
     * Optional notifier delay (env-gated). Sleeping here widens the
     * wake -> resubmit window: the waiter has already been dequeued
     * under q->lock but has not yet been signaled, so any concurrent
     * submit by another thread observes an empty queue while the
     * dispatched waiter is still parked. The dispatcher's spurious-
     * wake guard in the wait loop must keep behavior unchanged across
     * any delay value. Default 0 ns (no delay).
     */
    uint64_t delay_ns = die_disp_notifier_delay_ns();
    if (delay_ns > 0) {
        sleep_ns(delay_ns);
    }
    pthread_mutex_lock(&to_wake->cv_lock);
    to_wake->signaled = true;
    pthread_cond_signal(&to_wake->cv);
    pthread_mutex_unlock(&to_wake->cv_lock);
}
