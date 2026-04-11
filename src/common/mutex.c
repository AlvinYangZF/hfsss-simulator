#include "common/mutex.h"
#include <errno.h>
#include <pthread.h>
#include <time.h>

struct mutex_lock {
    pthread_mutex_t mutex;
};

int mutex_init(struct mutex *mtx)
{
    if (!mtx) {
        return HFSSS_ERR_INVAL;
    }

    memset(mtx, 0, sizeof(*mtx));
    mtx->owner = 0;
    mtx->recursion = 0;
    mtx->lock_count = 0;
    mtx->unlock_count = 0;

    struct mutex_lock *lock = (struct mutex_lock *)malloc(sizeof(struct mutex_lock));
    if (!lock) {
        return HFSSS_ERR_NOMEM;
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    mtx->lock = lock;

    return HFSSS_OK;
}

void mutex_cleanup(struct mutex *mtx)
{
    if (!mtx) {
        return;
    }

    if (mtx->lock) {
        struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
        pthread_mutex_destroy(&lock->mutex);
        free(lock);
    }

    memset(mtx, 0, sizeof(*mtx));
}

int mutex_trylock(struct mutex *mtx)
{
    if (!mtx) {
        return HFSSS_ERR_INVAL;
    }

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
    int ret = pthread_mutex_trylock(&lock->mutex);

    if (ret == 0) {
        mtx->lock_count++;
        return HFSSS_OK;
    } else {
        return HFSSS_ERR_BUSY;
    }
}

static u64 mutex_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

int mutex_lock(struct mutex *mtx, u64 timeout_ns)
{
    if (!mtx) {
        return HFSSS_ERR_INVAL;
    }

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;

    /* timeout_ns == 0 preserves the original blocking-forever semantics. */
    if (timeout_ns == 0) {
        pthread_mutex_lock(&lock->mutex);
        mtx->lock_count++;
        return HFSSS_OK;
    }

    /* Timed path: trylock in a short-sleep loop. pthread_mutex_timedlock
     * is not portable (absent on macOS), and we need to keep the mutex
     * recursive, so emulate via trylock + nanosleep. Poll resolution is
     * ~100us, good enough for tests and OOB wait-paths. Previously this
     * function ignored timeout_ns entirely and always blocked, which
     * silently broke every caller that relied on the timeout budget. */
    const u64 deadline = mutex_now_ns() + timeout_ns;
    const struct timespec sleep_step = {.tv_sec = 0, .tv_nsec = 100000};

    for (;;) {
        int rc = pthread_mutex_trylock(&lock->mutex);
        if (rc == 0) {
            mtx->lock_count++;
            return HFSSS_OK;
        }
        if (rc != EBUSY) {
            return HFSSS_ERR_IO;
        }
        if (mutex_now_ns() >= deadline) {
            return HFSSS_ERR_BUSY;
        }
        nanosleep(&sleep_step, NULL);
    }
}

int mutex_unlock(struct mutex *mtx)
{
    if (!mtx) {
        return HFSSS_ERR_INVAL;
    }

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
    pthread_mutex_unlock(&lock->mutex);
    mtx->unlock_count++;

    return HFSSS_OK;
}

void mutex_stats(struct mutex *mtx, u64 *lock_count, u64 *unlock_count)
{
    if (!mtx) {
        return;
    }

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
    pthread_mutex_lock(&lock->mutex);
    if (lock_count)
        *lock_count = mtx->lock_count;
    if (unlock_count)
        *unlock_count = mtx->unlock_count;
    pthread_mutex_unlock(&lock->mutex);
}
