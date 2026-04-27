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

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_MUTEX_H */
