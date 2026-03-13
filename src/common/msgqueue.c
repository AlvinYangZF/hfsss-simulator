#include "common/msgqueue.h"
#include <pthread.h>

struct msgqueue_lock {
    pthread_mutex_t mutex;
};

struct msgqueue_cond {
    pthread_cond_t cond;
};

int msg_queue_init(struct msg_queue *mq, u32 msg_size, u32 queue_len)
{
    if (!mq || msg_size == 0 || queue_len == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(mq, 0, sizeof(*mq));

    mq->buffer = (u8 *)calloc(queue_len, msg_size);
    if (!mq->buffer) {
        return HFSSS_ERR_NOMEM;
    }

    mq->msg_size = msg_size;
    mq->queue_len = queue_len;
    mq->head = 0;
    mq->tail = 0;
    mq->count = 0;
    mq->send_count = 0;
    mq->recv_count = 0;

    struct msgqueue_lock *lock = (struct msgqueue_lock *)malloc(sizeof(struct msgqueue_lock));
    if (!lock) {
        free(mq->buffer);
        return HFSSS_ERR_NOMEM;
    }
    pthread_mutex_init(&lock->mutex, NULL);
    mq->lock = lock;

    struct msgqueue_cond *not_empty = (struct msgqueue_cond *)malloc(sizeof(struct msgqueue_cond));
    struct msgqueue_cond *not_full = (struct msgqueue_cond *)malloc(sizeof(struct msgqueue_cond));
    if (!not_empty || !not_full) {
        free(lock);
        free(mq->buffer);
        free(not_empty);
        free(not_full);
        return HFSSS_ERR_NOMEM;
    }
    pthread_cond_init(&not_empty->cond, NULL);
    pthread_cond_init(&not_full->cond, NULL);
    mq->not_empty = not_empty;
    mq->not_full = not_full;

    return HFSSS_OK;
}

void msg_queue_cleanup(struct msg_queue *mq)
{
    if (!mq) {
        return;
    }

    if (mq->lock) {
        struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
        pthread_mutex_destroy(&lock->mutex);
        free(lock);
    }

    if (mq->not_empty) {
        struct msgqueue_cond *not_empty = (struct msgqueue_cond *)mq->not_empty;
        pthread_cond_destroy(&not_empty->cond);
        free(not_empty);
    }

    if (mq->not_full) {
        struct msgqueue_cond *not_full = (struct msgqueue_cond *)mq->not_full;
        pthread_cond_destroy(&not_full->cond);
        free(not_full);
    }

    if (mq->buffer) {
        free(mq->buffer);
    }

    memset(mq, 0, sizeof(*mq));
}

int msg_queue_trysend(struct msg_queue *mq, const void *msg)
{
    if (!mq || !msg) {
        return HFSSS_ERR_INVAL;
    }

    struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
    pthread_mutex_lock(&lock->mutex);

    int ret = HFSSS_ERR_BUSY;
    if (mq->count < mq->queue_len) {
        memcpy(&mq->buffer[mq->tail * mq->msg_size], msg, mq->msg_size);
        mq->tail = (mq->tail + 1) % mq->queue_len;
        mq->count++;
        mq->send_count++;
        ret = HFSSS_OK;

        struct msgqueue_cond *not_empty = (struct msgqueue_cond *)mq->not_empty;
        pthread_cond_signal(&not_empty->cond);
    }

    pthread_mutex_unlock(&lock->mutex);
    return ret;
}

int msg_queue_send(struct msg_queue *mq, const void *msg, u64 timeout_ns)
{
    if (!mq || !msg) {
        return HFSSS_ERR_INVAL;
    }

    struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
    struct msgqueue_cond *not_full = (struct msgqueue_cond *)mq->not_full;
    struct msgqueue_cond *not_empty = (struct msgqueue_cond *)mq->not_empty;

    pthread_mutex_lock(&lock->mutex);

    u64 start_time = get_time_ns();
    while (mq->count >= mq->queue_len) {
        if (timeout_ns == 0) {
            pthread_mutex_unlock(&lock->mutex);
            return HFSSS_ERR_BUSY;
        }

        u64 elapsed = get_time_ns() - start_time;
        if (elapsed >= timeout_ns) {
            pthread_mutex_unlock(&lock->mutex);
            return HFSSS_ERR_TIMEOUT;
        }

        u64 remaining = timeout_ns - elapsed;
        struct timespec ts;
        ts.tv_sec = remaining / 1000000000ULL;
        ts.tv_nsec = remaining % 1000000000ULL;
        pthread_cond_timedwait(&not_full->cond, &lock->mutex, &ts);
    }

    memcpy(&mq->buffer[mq->tail * mq->msg_size], msg, mq->msg_size);
    mq->tail = (mq->tail + 1) % mq->queue_len;
    mq->count++;
    mq->send_count++;

    pthread_cond_signal(&not_empty->cond);
    pthread_mutex_unlock(&lock->mutex);

    return HFSSS_OK;
}

int msg_queue_tryrecv(struct msg_queue *mq, void *msg)
{
    if (!mq || !msg) {
        return HFSSS_ERR_INVAL;
    }

    struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
    pthread_mutex_lock(&lock->mutex);

    int ret = HFSSS_ERR_NOENT;
    if (mq->count > 0) {
        memcpy(msg, &mq->buffer[mq->head * mq->msg_size], mq->msg_size);
        mq->head = (mq->head + 1) % mq->queue_len;
        mq->count--;
        mq->recv_count++;
        ret = HFSSS_OK;

        struct msgqueue_cond *not_full = (struct msgqueue_cond *)mq->not_full;
        pthread_cond_signal(&not_full->cond);
    }

    pthread_mutex_unlock(&lock->mutex);
    return ret;
}

int msg_queue_recv(struct msg_queue *mq, void *msg, u64 timeout_ns)
{
    if (!mq || !msg) {
        return HFSSS_ERR_INVAL;
    }

    struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
    struct msgqueue_cond *not_full = (struct msgqueue_cond *)mq->not_full;
    struct msgqueue_cond *not_empty = (struct msgqueue_cond *)mq->not_empty;

    pthread_mutex_lock(&lock->mutex);

    u64 start_time = get_time_ns();
    while (mq->count == 0) {
        if (timeout_ns == 0) {
            pthread_mutex_unlock(&lock->mutex);
            return HFSSS_ERR_NOENT;
        }

        u64 elapsed = get_time_ns() - start_time;
        if (elapsed >= timeout_ns) {
            pthread_mutex_unlock(&lock->mutex);
            return HFSSS_ERR_TIMEOUT;
        }

        u64 remaining = timeout_ns - elapsed;
        struct timespec ts;
        ts.tv_sec = remaining / 1000000000ULL;
        ts.tv_nsec = remaining % 1000000000ULL;
        pthread_cond_timedwait(&not_empty->cond, &lock->mutex, &ts);
    }

    memcpy(msg, &mq->buffer[mq->head * mq->msg_size], mq->msg_size);
    mq->head = (mq->head + 1) % mq->queue_len;
    mq->count--;
    mq->recv_count++;

    pthread_cond_signal(&not_full->cond);
    pthread_mutex_unlock(&lock->mutex);

    return HFSSS_OK;
}

u32 msg_queue_count(struct msg_queue *mq)
{
    if (!mq) {
        return 0;
    }

    struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
    pthread_mutex_lock(&lock->mutex);
    u32 count = mq->count;
    pthread_mutex_unlock(&lock->mutex);

    return count;
}

void msg_queue_stats(struct msg_queue *mq, u64 *send_count, u64 *recv_count)
{
    if (!mq) {
        return;
    }

    struct msgqueue_lock *lock = (struct msgqueue_lock *)mq->lock;
    pthread_mutex_lock(&lock->mutex);
    if (send_count) *send_count = mq->send_count;
    if (recv_count) *recv_count = mq->recv_count;
    pthread_mutex_unlock(&lock->mutex);
}
