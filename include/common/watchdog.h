#ifndef __HFSSS_WATCHDOG_H
#define __HFSSS_WATCHDOG_H

#include "common.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WATCHDOG_MAX_TASKS 32
#define WATCHDOG_NAME_MAX 32

/* Watchdog task states */
typedef enum {
    WATCHDOG_TASK_INACTIVE = 0,
    WATCHDOG_TASK_ACTIVE,
    WATCHDOG_TASK_TIMEOUT
} watchdog_task_state_t;

/* Watchdog callback type */
typedef void (*watchdog_timeout_cb_t)(int task_id, const char *task_name, void *user_data);

/* Watchdog task entry */
struct watchdog_task {
    char name[WATCHDOG_NAME_MAX];   /* Task name */
    watchdog_task_state_t state;      /* Current state */
    u64 timeout_ns;                   /* Timeout in nanoseconds */
    u64 last_feed_ns;                 /* Last feed time */
    void *user_data;                   /* User data for callback */
    int enabled;                       /* Whether this task is being monitored */
};

/* Watchdog context */
struct watchdog_ctx {
    struct watchdog_task tasks[WATCHDOG_MAX_TASKS];  /* Monitored tasks */
    int task_count;                                     /* Number of registered tasks */
    int running;                                        /* Watchdog thread running */
    pthread_t thread;                                   /* Watchdog thread */
    pthread_mutex_t mutex;                              /* Mutex for thread safety */
    pthread_cond_t cond;                                /* Condition variable */
    u64 check_interval_ns;                              /* Check interval in nanoseconds */
    watchdog_timeout_cb_t timeout_cb;                   /* Timeout callback */
    void *cb_user_data;                                 /* Callback user data */
    u64 timeout_count;                                  /* Total timeout count */
};

/* Function Prototypes */

/**
 * Initialize the watchdog
 *
 * @param ctx Pointer to watchdog context
 * @param check_interval_ns Interval between checks in nanoseconds
 * @param timeout_cb Callback function when a timeout occurs (can be NULL)
 * @param cb_user_data User data for the callback
 * @return HFSSS_OK on success, error code on failure
 */
int watchdog_init(struct watchdog_ctx *ctx, u64 check_interval_ns,
                  watchdog_timeout_cb_t timeout_cb, void *cb_user_data);

/**
 * Cleanup the watchdog
 *
 * @param ctx Pointer to watchdog context
 */
void watchdog_cleanup(struct watchdog_ctx *ctx);

/**
 * Start the watchdog monitoring thread
 *
 * @param ctx Pointer to watchdog context
 * @return HFSSS_OK on success, error code on failure
 */
int watchdog_start(struct watchdog_ctx *ctx);

/**
 * Stop the watchdog monitoring thread
 *
 * @param ctx Pointer to watchdog context
 */
void watchdog_stop(struct watchdog_ctx *ctx);

/**
 * Register a task with the watchdog
 *
 * @param ctx Pointer to watchdog context
 * @param name Name of the task
 * @param timeout_ns Timeout in nanoseconds
 * @param user_data User data for this task
 * @return Task ID (>=0) on success, error code on failure
 */
int watchdog_register_task(struct watchdog_ctx *ctx, const char *name,
                           u64 timeout_ns, void *user_data);

/**
 * Unregister a task from the watchdog
 *
 * @param ctx Pointer to watchdog context
 * @param task_id Task ID to unregister
 * @return HFSSS_OK on success, error code on failure
 */
int watchdog_unregister_task(struct watchdog_ctx *ctx, int task_id);

/**
 * Feed the watchdog for a task
 *
 * @param ctx Pointer to watchdog context
 * @param task_id Task ID to feed
 * @return HFSSS_OK on success, error code on failure
 */
int watchdog_feed(struct watchdog_ctx *ctx, int task_id);

/**
 * Enable monitoring for a task
 *
 * @param ctx Pointer to watchdog context
 * @param task_id Task ID to enable
 * @return HFSSS_OK on success, error code on failure
 */
int watchdog_enable_task(struct watchdog_ctx *ctx, int task_id);

/**
 * Disable monitoring for a task
 *
 * @param ctx Pointer to watchdog context
 * @param task_id Task ID to disable
 * @return HFSSS_OK on success, error code on failure
 */
int watchdog_disable_task(struct watchdog_ctx *ctx, int task_id);

/**
 * Get watchdog statistics
 *
 * @param ctx Pointer to watchdog context
 * @param timeout_count Pointer to store total timeout count
 * @param active_tasks Pointer to store number of active tasks
 */
void watchdog_stats(struct watchdog_ctx *ctx, u64 *timeout_count, int *active_tasks);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_WATCHDOG_H */
