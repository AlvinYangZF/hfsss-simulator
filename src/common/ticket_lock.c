/*
 * FIFO-fair ticket lock implementation.
 * See include/common/ticket_lock.h for the contract.
 */

#include "common/ticket_lock.h"
#include <sched.h>

void ticket_lock_init(struct ticket_lock *l)
{
    if (!l) {
        return;
    }
    atomic_store(&l->ticket, 0);
    atomic_store(&l->serving, 0);
}

void ticket_lock_lock(struct ticket_lock *l)
{
    u64 my = atomic_fetch_add(&l->ticket, 1);
    while (atomic_load(&l->serving) != my) {
        sched_yield();
    }
}

void ticket_lock_unlock(struct ticket_lock *l)
{
    atomic_fetch_add(&l->serving, 1);
}

bool ticket_lock_try_lock(struct ticket_lock *l)
{
    u64 serving = atomic_load(&l->serving);
    u64 ticket = atomic_load(&l->ticket);
    /* Only succeed when no one is queued. */
    if (serving != ticket) {
        return false;
    }
    u64 next = ticket + 1;
    if (atomic_compare_exchange_strong(&l->ticket, &ticket, next)) {
        /* We claimed the ticket; now we must wait until served. */
        while (atomic_load(&l->serving) != ticket) {
            sched_yield();
        }
        return true;
    }
    return false;
}
