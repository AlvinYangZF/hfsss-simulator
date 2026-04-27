#ifndef __HFSSS_MUTEX_H
#define __HFSSS_MUTEX_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mutex Context */
struct mutex {
    void *lock;
    u32 owner;
    u32 recursion;
    u64 lock_count;
    u64 unlock_count;
};

/* Function Prototypes */
int mutex_init(struct mutex *mtx);
void mutex_cleanup(struct mutex *mtx);
int mutex_lock(struct mutex *mtx, u64 timeout_ns);
int mutex_trylock(struct mutex *mtx);
int mutex_unlock(struct mutex *mtx);
void mutex_stats(struct mutex *mtx, u64 *lock_count, u64 *unlock_count);

/* Wait on a condition variable while holding mtx. Atomically releases
 * mtx, blocks until cv is signaled, then re-acquires mtx before
 * returning. Caller must hold mtx exactly once (no nested locks) — the
 * underlying mutex is recursive but cond_wait only drops one level.
 * cv is treated as opaque (a pthread_cond_t * — caller includes pthread.h). */
int mutex_cond_wait(struct mutex *mtx, void *cv);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_MUTEX_H */
