/*
 * FTL die-busy wait queue (per-die multi-tier).
 *
 * The dispatcher owns one wait queue per (channel, chip, die). FTL workers
 * that observe HFSSS_ERR_BUSY from cmd_engine block on this queue until the
 * cmd_engine die-ready notifier wakes them. The dispatcher installs itself as
 * dev->die_ready_notifier on create and removes itself on destroy.
 *
 * See docs/superpowers/specs/2026-04-30-ftl-die-busy-waitqueue-design.md.
 */
#ifndef __HFSSS_FTL_DIE_DISPATCHER_H
#define __HFSSS_FTL_DIE_DISPATCHER_H

#include "common/common.h"
#include "media/nand.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Priority class assigned by the caller at submit time. Each value maps to
 * exactly one wait-queue tier (T1..T4); two tiers (T2, T3) host two priority
 * classes that share fair-queueing weight.
 */
typedef enum {
    DIE_PRIO_HOST_READ,    /* T2 share */
    DIE_PRIO_HOST_WRITE,   /* T3 share */
    DIE_PRIO_GC_CRITICAL,  /* T1 strict pre-emption */
    DIE_PRIO_GC_FORCE,     /* T2 share — read-disturb / WL trigger */
    DIE_PRIO_GC_NORMAL,    /* T3 share — host-write-driven block release */
    DIE_PRIO_GC_IDLE,      /* T4 — yields under host burst */
    DIE_PRIO_COUNT
} die_priority_t;

/*
 * GC kick reason. Carried through the GC pipeline so the dispatcher submit
 * site can map the right priority class via die_dispatcher_prio_for_gc.
 */
typedef enum {
    GC_TRIGGER_FREE_SB_LOW,    /* DIE_PRIO_GC_CRITICAL */
    GC_TRIGGER_READ_DISTURB,   /* DIE_PRIO_GC_FORCE */
    GC_TRIGGER_WEAR_LEVELING,  /* DIE_PRIO_GC_FORCE */
    GC_TRIGGER_HOST_WRITE,     /* DIE_PRIO_GC_NORMAL */
    GC_TRIGGER_IDLE            /* DIE_PRIO_GC_IDLE */
} gc_trigger_t;

struct die_dispatcher;

/*
 * Lifecycle. Create installs dev->die_ready_notifier and dev->die_ready_ctx;
 * destroy uninstalls (sets both back to NULL) and frees state.
 */
struct die_dispatcher *die_dispatcher_create(struct nand_device *dev);
void                   die_dispatcher_destroy(struct die_dispatcher *d);

/*
 * Block until the target die is dispatched to the caller. On return the
 * caller holds a logical "go ahead" and is expected to immediately invoke
 * cmd_engine submit. If cmd_engine returns HFSSS_ERR_BUSY, the caller calls
 * die_dispatcher_wait again to re-queue.
 *
 * Returns 0 on signaled wake, ETIMEDOUT on max_wait_ms expiry, or a negative
 * HFSSS_ERR_* code on shutdown / invalid input. max_wait_ms == 0 means
 * "no timeout".
 *
 * Body lands with the multi-thread integration; this header pins the
 * signature so dependent test code can compile.
 */
int die_dispatcher_wait(struct die_dispatcher *d,
                        u32 ch, u32 chip, u32 die,
                        die_priority_t prio,
                        u32 max_wait_ms);

/*
 * Notifier hook installed on dev->die_ready_notifier. Wakes one waiter on
 * the matching per-die queue. Body lands with the multi-thread integration.
 */
void die_dispatcher_on_die_ready(struct nand_device *dev,
                                 u32 ch, u32 chip, u32 die);

/*
 * Single source of truth for gc_trigger_t -> die_priority_t mapping.
 */
die_priority_t die_dispatcher_prio_for_gc(gc_trigger_t t);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_FTL_DIE_DISPATCHER_H */
