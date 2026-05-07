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
#include <stdlib.h>
#include <string.h>

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

void die_dispatcher_destroy(struct die_dispatcher *d)
{
    if (!d) {
        return;
    }
    if (d->dev) {
        d->dev->die_ready_notifier = NULL;
        d->dev->die_ready_ctx = NULL;
    }
    if (d->queues) {
        for (u32 i = 0; i < d->queue_count; i++) {
            die_waitqueue_cleanup(&d->queues[i]);
        }
        free(d->queues);
    }
    free(d);
}

/* ------------------------------------------------------------------ */
/* Wait / notifier stubs (bodies land with the multi-thread task)     */
/* ------------------------------------------------------------------ */

int die_dispatcher_wait(struct die_dispatcher *d,
                        u32 ch, u32 chip, u32 die,
                        die_priority_t prio,
                        u32 max_wait_ms)
{
    (void)d;
    (void)ch;
    (void)chip;
    (void)die;
    (void)prio;
    (void)max_wait_ms;
    return HFSSS_ERR_NOTSUPP;
}

void die_dispatcher_on_die_ready(struct nand_device *dev,
                                 u32 ch, u32 chip, u32 die)
{
    (void)dev;
    (void)ch;
    (void)chip;
    (void)die;
}
