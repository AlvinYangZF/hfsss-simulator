/*
 * FIFO-fair ticket lock for short-held critical sections.
 *
 * Two atomic counters: ticket (next to issue) and serving (currently
 * running). lock() takes a ticket and spins until serving == ticket;
 * unlock() increments serving. Order of acquisition is the order in
 * which lock() callers issued tickets — exactly FIFO with respect to
 * the atomic_fetch_add.
 *
 * Sized for cmd_engine's die_lock / channel_lock contention windows
 * (microseconds to a few ms). Not appropriate for sleep-on-lock
 * workloads; use struct mutex for those.
 *
 * Callers must not invoke blocking operations (sleep, condvar wait,
 * or any function that may internally sleep) while holding this lock
 * — including via callbacks installed from outside subsystems
 * (e.g., the die_ready_notifier hook). The lock spin-busys until
 * served, so a holder that sleeps forces every queued waiter to
 * burn CPU for the full sleep duration.
 *
 * See docs/superpowers/specs/2026-05-08-cmd-engine-ticket-lock-design.md.
 */
#ifndef HFSSS_TICKET_LOCK_H
#define HFSSS_TICKET_LOCK_H

#include "common/common.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ticket_lock {
    _Atomic u64 ticket;
    _Atomic u64 serving;
};

void  ticket_lock_init(struct ticket_lock *l);
void  ticket_lock_lock(struct ticket_lock *l);
void  ticket_lock_unlock(struct ticket_lock *l);
bool  ticket_lock_try_lock(struct ticket_lock *l);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_TICKET_LOCK_H */
