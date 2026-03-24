#include "controller/qos.h"
#include <stdlib.h>
#include <string.h>

#define DWRR_BASE_QUANTUM 10

/* Find queue index by nsid. Returns -1 if not found. */
static int dwrr_find_queue(struct dwrr_scheduler *sched, u32 nsid)
{
    for (u32 i = 0; i < QOS_MAX_NAMESPACES; i++) {
        if (sched->queues[i].active && sched->queues[i].nsid == nsid) {
            return (int)i;
        }
    }
    return -1;
}

/* Find a free slot. Returns -1 if none available. */
static int dwrr_find_free_slot(struct dwrr_scheduler *sched)
{
    for (u32 i = 0; i < QOS_MAX_NAMESPACES; i++) {
        if (!sched->queues[i].active) {
            return (int)i;
        }
    }
    return -1;
}

int dwrr_init(struct dwrr_scheduler *sched, u32 max_outstanding)
{
    int ret;

    if (!sched) {
        return HFSSS_ERR_INVAL;
    }

    memset(sched, 0, sizeof(*sched));

    ret = mutex_init(&sched->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    sched->base_quantum = DWRR_BASE_QUANTUM;
    sched->max_outstanding = max_outstanding;
    sched->current_idx = 0;
    sched->throttle_factor = 1.0;
    sched->initialized = true;

    return HFSSS_OK;
}

void dwrr_cleanup(struct dwrr_scheduler *sched)
{
    if (!sched || !sched->initialized) {
        return;
    }

    mutex_lock(&sched->lock, 0);
    mutex_unlock(&sched->lock);

    mutex_cleanup(&sched->lock);
    memset(sched, 0, sizeof(*sched));
}

int dwrr_queue_create(struct dwrr_scheduler *sched, u32 nsid, u32 weight)
{
    int idx;

    if (!sched || !sched->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (weight == 0) {
        weight = QOS_DEFAULT_WEIGHT;
    }

    mutex_lock(&sched->lock, 0);

    /* Check for duplicate nsid */
    if (dwrr_find_queue(sched, nsid) >= 0) {
        mutex_unlock(&sched->lock);
        return HFSSS_ERR_EXIST;
    }

    idx = dwrr_find_free_slot(sched);
    if (idx < 0) {
        mutex_unlock(&sched->lock);
        return HFSSS_ERR_NOSPC;
    }

    struct dwrr_queue *q = &sched->queues[idx];
    q->nsid = nsid;
    q->weight = weight;
    q->deficit = 0;
    q->pending_cmds = 0;
    q->dispatched_cmds = 0;
    q->active = true;

    sched->active_count++;

    mutex_unlock(&sched->lock);
    return HFSSS_OK;
}

int dwrr_queue_delete(struct dwrr_scheduler *sched, u32 nsid)
{
    int idx;

    if (!sched || !sched->initialized) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&sched->lock, 0);

    idx = dwrr_find_queue(sched, nsid);
    if (idx < 0) {
        mutex_unlock(&sched->lock);
        return HFSSS_ERR_NOENT;
    }

    memset(&sched->queues[idx], 0, sizeof(struct dwrr_queue));
    sched->active_count--;

    mutex_unlock(&sched->lock);
    return HFSSS_OK;
}

int dwrr_enqueue(struct dwrr_scheduler *sched, u32 nsid)
{
    int idx;

    if (!sched || !sched->initialized) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&sched->lock, 0);

    idx = dwrr_find_queue(sched, nsid);
    if (idx < 0) {
        mutex_unlock(&sched->lock);
        return HFSSS_ERR_NOENT;
    }

    sched->queues[idx].pending_cmds++;

    mutex_unlock(&sched->lock);
    return HFSSS_OK;
}

int dwrr_dequeue(struct dwrr_scheduler *sched, u32 *nsid_out)
{
    if (!sched || !sched->initialized || !nsid_out) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&sched->lock, 0);

    if (sched->active_count == 0) {
        mutex_unlock(&sched->lock);
        return HFSSS_ERR_NOENT;
    }

    /* Effective quantum considers throttle factor */
    u32 effective_quantum = (u32)(sched->base_quantum * sched->throttle_factor);
    if (effective_quantum == 0) {
        effective_quantum = 1;
    }

    /* DWRR: first try to find a queue with positive deficit.
     * If none found, replenish ALL active queues proportional to weight,
     * then scan again. */
    for (u32 pass = 0; pass < 2; pass++) {
        /* Scan for a queue with deficit > 0 and pending commands */
        for (u32 i = 0; i < QOS_MAX_NAMESPACES; i++) {
            u32 idx = (sched->current_idx + i) % QOS_MAX_NAMESPACES;
            struct dwrr_queue *q = &sched->queues[idx];

            if (!q->active || q->pending_cmds == 0) {
                continue;
            }

            if (q->deficit > 0) {
                u32 cost = 1;
                q->deficit -= (s32)cost;
                q->pending_cmds--;
                q->dispatched_cmds++;
                *nsid_out = q->nsid;
                /* Stay on this queue if it still has deficit and pending;
                 * advance only when deficit exhausted or queue empty. */
                if (q->deficit <= 0 || q->pending_cmds == 0) {
                    sched->current_idx = (idx + 1) % QOS_MAX_NAMESPACES;
                }
                mutex_unlock(&sched->lock);
                return HFSSS_OK;
            }
        }

        /* No queue had positive deficit — replenish all active queues */
        if (pass == 0) {
            for (u32 i = 0; i < QOS_MAX_NAMESPACES; i++) {
                struct dwrr_queue *q = &sched->queues[i];
                if (q->active && q->pending_cmds > 0) {
                    q->deficit += (s32)(effective_quantum * q->weight);
                }
            }
        }
    }

    mutex_unlock(&sched->lock);
    return HFSSS_ERR_AGAIN;
}

u32 dwrr_command_cost(bool is_write)
{
    return is_write ? 2 : 1;
}

void dwrr_set_throttle_factor(struct dwrr_scheduler *sched, double factor)
{
    if (!sched || !sched->initialized) {
        return;
    }

    mutex_lock(&sched->lock, 0);

    if (factor < 0.0) {
        factor = 0.0;
    }
    if (factor > 1.0) {
        factor = 1.0;
    }
    sched->throttle_factor = factor;

    mutex_unlock(&sched->lock);
}

bool dwrr_has_pending(struct dwrr_scheduler *sched)
{
    if (!sched || !sched->initialized) {
        return false;
    }

    mutex_lock(&sched->lock, 0);

    for (u32 i = 0; i < QOS_MAX_NAMESPACES; i++) {
        if (sched->queues[i].active && sched->queues[i].pending_cmds > 0) {
            mutex_unlock(&sched->lock);
            return true;
        }
    }

    mutex_unlock(&sched->lock);
    return false;
}

void dwrr_get_stats(struct dwrr_scheduler *sched, u32 nsid,
                    u32 *pending, u32 *dispatched)
{
    if (!sched || !sched->initialized) {
        if (pending) *pending = 0;
        if (dispatched) *dispatched = 0;
        return;
    }

    mutex_lock(&sched->lock, 0);

    int idx = dwrr_find_queue(sched, nsid);
    if (idx < 0) {
        if (pending) *pending = 0;
        if (dispatched) *dispatched = 0;
    } else {
        if (pending) *pending = sched->queues[idx].pending_cmds;
        if (dispatched) *dispatched = sched->queues[idx].dispatched_cmds;
    }

    mutex_unlock(&sched->lock);
}
