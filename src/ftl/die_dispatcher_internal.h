/*
 * Internal types and helpers for the FTL die-busy wait queue.
 *
 * Visible only to die_dispatcher.c and the unit-test translation unit so the
 * tests can drive the data structure directly without going through the
 * full create/destroy/notifier path.
 */
#ifndef __HFSSS_FTL_DIE_DISPATCHER_INTERNAL_H
#define __HFSSS_FTL_DIE_DISPATCHER_INTERNAL_H

#include "common/common.h"
#include "ftl/die_dispatcher.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of wait-queue tiers (T1..T4). */
#define DIE_DISPATCH_TIER_COUNT 4

/* Slot count for tiers that share two priority classes (T2 and T3). */
#define DIE_DISPATCH_WFQ_SLOT_COUNT 2

/* Default WFQ quantum per slot (1:1 share). */
#define DIE_DISPATCH_WFQ_DEFAULT_QUANTUM 1

/* Default EMA smoothing factor (alpha = 1/8). */
#define DIE_DISPATCH_EMA_DEFAULT_ALPHA 0.125

/* Default host-pressure thresholds (1 ms each). */
#define DIE_DISPATCH_DEFAULT_YIELD_NS  ((u64)1000000)
#define DIE_DISPATCH_DEFAULT_GRACE_NS  ((u64)1000000)

/*
 * Intrusive doubly-linked list head. Every node is also a list_head — a
 * detached node has list.next == list.prev == &list (self-loop), which
 * dist_list_empty() relies on.
 */
struct dispatch_list {
    struct dispatch_list *next;
    struct dispatch_list *prev;
};

static inline void dispatch_list_init(struct dispatch_list *l)
{
    l->next = l;
    l->prev = l;
}

static inline bool dispatch_list_empty(const struct dispatch_list *head)
{
    return head->next == head;
}

static inline void dispatch_list_add_tail(struct dispatch_list *head,
                                          struct dispatch_list *node)
{
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

static inline void dispatch_list_del(struct dispatch_list *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

/*
 * Per-waiter record. Embedded in the caller's stack frame (no allocation).
 * The cv / cv_lock pair is what the caller blocks on; the list link threads
 * the waiter through one of the per-die tier lists.
 */
struct die_waiter {
    struct dispatch_list list;     /* member of waitqueue.tier[N] */
    pthread_cond_t       cv;
    pthread_mutex_t      cv_lock;
    bool                 signaled;
    bool                 shutdown;
    die_priority_t       prio;
};

/*
 * Per-(ch,chip,die) wait queue. Tier 0..3 correspond to T1..T4 in the spec.
 */
struct die_waitqueue {
    pthread_mutex_t      lock;
    struct dispatch_list tier[DIE_DISPATCH_TIER_COUNT];
    int32_t              wfq_deficit_t2[DIE_DISPATCH_WFQ_SLOT_COUNT];
    int32_t              wfq_deficit_t3[DIE_DISPATCH_WFQ_SLOT_COUNT];
    int32_t              wfq_quantum_t2[DIE_DISPATCH_WFQ_SLOT_COUNT];
    int32_t              wfq_quantum_t3[DIE_DISPATCH_WFQ_SLOT_COUNT];
    double               ema_iat_ns;
    u64                  last_host_ts_ns;
    double               ema_alpha;
    u64                  yield_threshold_ns;
    u64                  burst_grace_ns;
};

/*
 * Aggregate. Holds one waitqueue per (ch, chip, die) flattened into a
 * dense array; geometry is captured at create time so we can index into it
 * by linear offset.
 */
struct die_dispatcher {
    struct nand_device   *dev;
    struct die_waitqueue *queues;
    u32                   queue_count;
    u32                   chips_per_channel;
    u32                   dies_per_chip;
};

/* Lifecycle helpers (also used directly by unit tests). */
void die_waitqueue_init(struct die_waitqueue *q);
void die_waitqueue_cleanup(struct die_waitqueue *q);

void die_waiter_init(struct die_waiter *w, die_priority_t prio);
void die_waiter_cleanup(struct die_waiter *w);

/* Tier-mapping helpers. tier index in [0,3], slot index in [0,1] or -1. */
int die_priority_tier(die_priority_t prio);
int die_priority_slot(die_priority_t prio);

/* List-mutation primitives, all called with q->lock held. */
void                die_waitqueue_enqueue(struct die_waitqueue *q,
                                          struct die_waiter *w);
struct die_waiter  *die_waitqueue_pop_next(struct die_waitqueue *q);

/* Host pressure tracking. */
void die_waitqueue_record_host_io(struct die_waitqueue *q, u64 now_ns);
bool die_waitqueue_host_bursting(struct die_waitqueue *q, u64 now_ns);

/*
 * Test-only: reset the cached env values for the fault-injection knobs
 * (HFSSS_DIE_DISP_FORCE_BUSY, HFSSS_DIE_DISP_NOTIFIER_DELAY_NS). Tests
 * call this after setenv()/unsetenv() so the next probe re-reads the
 * environment. Production code never calls this.
 */
void die_dispatcher_reset_env_cache_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_FTL_DIE_DISPATCHER_INTERNAL_H */
