#include "common/mutex.h"
#include <pthread.h>

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

int mutex_lock(struct mutex *mtx, u64 timeout_ns)
{
    if (!mtx) {
        return HFSSS_ERR_INVAL;
    }

    (void)timeout_ns; /* pthread mutex doesn't support timed lock easily with recursive */

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
    pthread_mutex_lock(&lock->mutex);
    mtx->lock_count++;

    return HFSSS_OK;
}

int mutex_unlock(struct mutex *mtx)
{
    if (!mtx) {
        return HFSSS_ERR_INVAL;
    }

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
    /*
     * The debug counter must be bumped while the mutex is still held.
     * mutex_stats reads both counters under the same mutex, so if we
     * unlocked before the increment another thread could observe a
     * torn pair (lock_count > unlock_count + 1). TSAN also correctly
     * flagged the unlocked write as a data race with concurrent
     * acquirers.
     */
    mtx->unlock_count++;
    pthread_mutex_unlock(&lock->mutex);

    return HFSSS_OK;
}

void mutex_stats(struct mutex *mtx, u64 *lock_count, u64 *unlock_count)
{
    if (!mtx) {
        return;
    }

    struct mutex_lock *lock = (struct mutex_lock *)mtx->lock;
    pthread_mutex_lock(&lock->mutex);
    if (lock_count) *lock_count = mtx->lock_count;
    if (unlock_count) *unlock_count = mtx->unlock_count;
    pthread_mutex_unlock(&lock->mutex);
}
