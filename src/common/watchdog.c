#include "common/watchdog.h"

static void *watchdog_thread(void *arg);

int watchdog_init(struct watchdog_ctx *ctx, u64 check_interval_ns,
                  watchdog_timeout_cb_t timeout_cb, void *cb_user_data)
{
    if (!ctx || check_interval_ns == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->check_interval_ns = check_interval_ns;
    ctx->timeout_cb = timeout_cb;
    ctx->cb_user_data = cb_user_data;
    ctx->running = 0;
    ctx->task_count = 0;
    ctx->timeout_count = 0;

    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);

    return HFSSS_OK;
}

void watchdog_cleanup(struct watchdog_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    watchdog_stop(ctx);

    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);

    memset(ctx, 0, sizeof(*ctx));
}

int watchdog_start(struct watchdog_ctx *ctx)
{
    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->mutex);

    if (ctx->running) {
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_OK;
    }

    ctx->running = 1;

    /* Initialize last_feed_ns for all active tasks */
    u64 now = get_time_ns();
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (ctx->tasks[i].state == WATCHDOG_TASK_ACTIVE) {
            ctx->tasks[i].last_feed_ns = now;
        }
    }

    pthread_mutex_unlock(&ctx->mutex);

    if (pthread_create(&ctx->thread, NULL, watchdog_thread, ctx) != 0) {
        pthread_mutex_lock(&ctx->mutex);
        ctx->running = 0;
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_ERR;
    }

    return HFSSS_OK;
}

void watchdog_stop(struct watchdog_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->mutex);

    if (!ctx->running) {
        pthread_mutex_unlock(&ctx->mutex);
        return;
    }

    ctx->running = 0;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);

    pthread_join(ctx->thread, NULL);
}

int watchdog_register_task(struct watchdog_ctx *ctx, const char *name,
                           u64 timeout_ns, void *user_data)
{
    if (!ctx || !name || timeout_ns == 0) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->mutex);

    /* Find an empty slot */
    int task_id = -1;
    for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
        if (ctx->tasks[i].state == WATCHDOG_TASK_INACTIVE) {
            task_id = i;
            break;
        }
    }

    if (task_id < 0) {
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_ERR_NOSPC;
    }

    /* Initialize the task */
    struct watchdog_task *task = &ctx->tasks[task_id];
    strncpy(task->name, name, WATCHDOG_NAME_MAX - 1);
    task->name[WATCHDOG_NAME_MAX - 1] = '\0';
    task->timeout_ns = timeout_ns;
    task->last_feed_ns = get_time_ns();
    task->user_data = user_data;
    task->state = WATCHDOG_TASK_ACTIVE;
    task->enabled = 1;

    ctx->task_count++;

    pthread_mutex_unlock(&ctx->mutex);

    return task_id;
}

int watchdog_unregister_task(struct watchdog_ctx *ctx, int task_id)
{
    if (!ctx || task_id < 0 || task_id >= WATCHDOG_MAX_TASKS) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->mutex);

    struct watchdog_task *task = &ctx->tasks[task_id];
    if (task->state == WATCHDOG_TASK_INACTIVE) {
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_ERR_NOENT;
    }

    task->state = WATCHDOG_TASK_INACTIVE;
    task->enabled = 0;
    ctx->task_count--;

    pthread_mutex_unlock(&ctx->mutex);

    return HFSSS_OK;
}

int watchdog_feed(struct watchdog_ctx *ctx, int task_id)
{
    if (!ctx || task_id < 0 || task_id >= WATCHDOG_MAX_TASKS) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->mutex);

    struct watchdog_task *task = &ctx->tasks[task_id];
    if (task->state != WATCHDOG_TASK_ACTIVE) {
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_ERR_NOENT;
    }

    task->last_feed_ns = get_time_ns();

    /* Reset timeout state if it was timed out */
    if (task->state == WATCHDOG_TASK_TIMEOUT) {
        task->state = WATCHDOG_TASK_ACTIVE;
    }

    pthread_mutex_unlock(&ctx->mutex);

    return HFSSS_OK;
}

int watchdog_enable_task(struct watchdog_ctx *ctx, int task_id)
{
    if (!ctx || task_id < 0 || task_id >= WATCHDOG_MAX_TASKS) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->mutex);

    struct watchdog_task *task = &ctx->tasks[task_id];
    if (task->state == WATCHDOG_TASK_INACTIVE) {
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_ERR_NOENT;
    }

    task->enabled = 1;
    task->last_feed_ns = get_time_ns();

    pthread_mutex_unlock(&ctx->mutex);

    return HFSSS_OK;
}

int watchdog_disable_task(struct watchdog_ctx *ctx, int task_id)
{
    if (!ctx || task_id < 0 || task_id >= WATCHDOG_MAX_TASKS) {
        return HFSSS_ERR_INVAL;
    }

    pthread_mutex_lock(&ctx->mutex);

    struct watchdog_task *task = &ctx->tasks[task_id];
    if (task->state == WATCHDOG_TASK_INACTIVE) {
        pthread_mutex_unlock(&ctx->mutex);
        return HFSSS_ERR_NOENT;
    }

    task->enabled = 0;

    pthread_mutex_unlock(&ctx->mutex);

    return HFSSS_OK;
}

void watchdog_stats(struct watchdog_ctx *ctx, u64 *timeout_count, int *active_tasks)
{
    if (!ctx) {
        return;
    }

    pthread_mutex_lock(&ctx->mutex);

    if (timeout_count) {
        *timeout_count = ctx->timeout_count;
    }

    if (active_tasks) {
        *active_tasks = 0;
        for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
            if (ctx->tasks[i].state == WATCHDOG_TASK_ACTIVE && ctx->tasks[i].enabled) {
                (*active_tasks)++;
            }
        }
    }

    pthread_mutex_unlock(&ctx->mutex);
}

static void *watchdog_thread(void *arg)
{
    struct watchdog_ctx *ctx = (struct watchdog_ctx *)arg;

    pthread_mutex_lock(&ctx->mutex);

    while (ctx->running) {
        u64 now = get_time_ns();

        /* Check all tasks */
        for (int i = 0; i < WATCHDOG_MAX_TASKS; i++) {
            struct watchdog_task *task = &ctx->tasks[i];

            if (task->state == WATCHDOG_TASK_ACTIVE && task->enabled) {
                u64 elapsed = now - task->last_feed_ns;

                if (elapsed > task->timeout_ns) {
                    /* Task timed out */
                    task->state = WATCHDOG_TASK_TIMEOUT;
                    ctx->timeout_count++;

                    /* Call the timeout callback if set */
                    if (ctx->timeout_cb) {
                        /* Unlock mutex before calling callback */
                        pthread_mutex_unlock(&ctx->mutex);
                        ctx->timeout_cb(i, task->name, task->user_data);
                        pthread_mutex_lock(&ctx->mutex);
                    }
                }
            }
        }

        /* Wait for the check interval or until stopped */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ctx->check_interval_ns / 1000000000ULL;
        ts.tv_nsec += ctx->check_interval_ns % 1000000000ULL;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }

        pthread_cond_timedwait(&ctx->cond, &ctx->mutex, &ts);
    }

    pthread_mutex_unlock(&ctx->mutex);

    return NULL;
}
