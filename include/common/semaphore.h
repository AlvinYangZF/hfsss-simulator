#ifndef __HFSSS_SEMAPHORE_H
#define __HFSSS_SEMAPHORE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Semaphore Context */
struct semaphore {
    int count;
    void *lock;
    void *cond;
    u64 wait_count;
    u64 signal_count;
};

/* Function Prototypes */
int semaphore_init(struct semaphore *sem, int initial_count);
void semaphore_cleanup(struct semaphore *sem);
int semaphore_take(struct semaphore *sem, u64 timeout_ns);
int semaphore_trytake(struct semaphore *sem);
int semaphore_give(struct semaphore *sem);
int semaphore_get_count(struct semaphore *sem);
void semaphore_stats(struct semaphore *sem, u64 *wait_count, u64 *signal_count);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_SEMAPHORE_H */
