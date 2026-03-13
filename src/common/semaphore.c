#include "common/semaphore.h"
#include <pthread.h>

struct sem_lock {
    pthread_mutex_t mutex;
};

struct sem_cond {
    pthread_cond_t cond;
};

int semaphore_init(struct semaphore *sem, int initial_count)
{
    if (!sem) {
        return HFSSS_ERR_INVAL;
    }

    memset(sem, 0, sizeof(*sem));
    sem->count = initial_count;

    struct sem_lock *lock = (struct sem_lock *)malloc(sizeof(struct sem_lock));
    if (!lock) {
        return HFSSS_ERR_NOMEM;
    }
    pthread_mutex_init(&lock->mutex, NULL);
    sem->lock = lock;

    struct sem_cond *cond = (struct sem_cond *)malloc(sizeof(struct sem_cond));
    if (!cond) {
        free(lock);
        return HFSSS_ERR_NOMEM;
    }
    pthread_cond_init(&cond->cond, NULL);
    sem->cond = cond;

    sem->wait_count = 0;
    sem->signal_count = 0;

    return HFSSS_OK;
}

void semaphore_cleanup(struct semaphore *sem)
{
    if (!sem) {
        return;
    }

    if (sem->lock) {
        struct sem_lock *lock = (struct sem_lock *)sem->lock;
        pthread_mutex_destroy(&lock->mutex);
        free(lock);
    }

    if (sem->cond) {
        struct sem_cond *cond = (struct sem_cond *)sem->cond;
        pthread_cond_destroy(&cond->cond);
        free(cond);
    }

    memset(sem, 0, sizeof(*sem));
}

int semaphore_trytake(struct semaphore *sem)
{
    if (!sem) {
        return HFSSS_ERR_INVAL;
    }

    struct sem_lock *lock = (struct sem_lock *)sem->lock;
    pthread_mutex_lock(&lock->mutex);

    int ret = HFSSS_ERR_BUSY;
    if (sem->count > 0) {
        sem->count--;
        ret = HFSSS_OK;
    }

    pthread_mutex_unlock(&lock->mutex);
    return ret;
}

int semaphore_take(struct semaphore *sem, u64 timeout_ns)
{
    if (!sem) {
        return HFSSS_ERR_INVAL;
    }

    struct sem_lock *lock = (struct sem_lock *)sem->lock;
    struct sem_cond *cond = (struct sem_cond *)sem->cond;
    pthread_mutex_lock(&lock->mutex);

    u64 start_time = get_time_ns();
    while (sem->count <= 0) {
        if (timeout_ns == 0) {
            pthread_mutex_unlock(&lock->mutex);
            return HFSSS_ERR_BUSY;
        }

        u64 elapsed = get_time_ns() - start_time;
        if (elapsed >= timeout_ns) {
            pthread_mutex_unlock(&lock->mutex);
            return HFSSS_ERR_TIMEOUT;
        }

        sem->wait_count++;
        u64 remaining = timeout_ns - elapsed;
        struct timespec ts;
        ts.tv_sec = remaining / 1000000000ULL;
        ts.tv_nsec = remaining % 1000000000ULL;
        pthread_cond_timedwait(&cond->cond, &lock->mutex, &ts);
    }

    sem->count--;
    pthread_mutex_unlock(&lock->mutex);
    return HFSSS_OK;
}

int semaphore_give(struct semaphore *sem)
{
    if (!sem) {
        return HFSSS_ERR_INVAL;
    }

    struct sem_lock *lock = (struct sem_lock *)sem->lock;
    struct sem_cond *cond = (struct sem_cond *)sem->cond;
    pthread_mutex_lock(&lock->mutex);

    sem->count++;
    sem->signal_count++;
    pthread_cond_signal(&cond->cond);

    pthread_mutex_unlock(&lock->mutex);
    return HFSSS_OK;
}

int semaphore_get_count(struct semaphore *sem)
{
    if (!sem) {
        return -1;
    }

    struct sem_lock *lock = (struct sem_lock *)sem->lock;
    pthread_mutex_lock(&lock->mutex);
    int count = sem->count;
    pthread_mutex_unlock(&lock->mutex);

    return count;
}

void semaphore_stats(struct semaphore *sem, u64 *wait_count, u64 *signal_count)
{
    if (!sem) {
        return;
    }

    struct sem_lock *lock = (struct sem_lock *)sem->lock;
    pthread_mutex_lock(&lock->mutex);
    if (wait_count) *wait_count = sem->wait_count;
    if (signal_count) *signal_count = sem->signal_count;
    pthread_mutex_unlock(&lock->mutex);
}
