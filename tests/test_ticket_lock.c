/*
 * Ticket lock unit tests — L1 data structure layer.
 *
 * Coverage:
 *   - Single-thread acquire / release
 *   - Counters tracking after multiple acquires
 *   - try_lock on uncontended and contended lock
 *   - FIFO acquisition order under multi-thread contention
 *   - Stress: 64 threads × 10000 iterations (no lost increments, no hang)
 */

#include "common/ticket_lock.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT(cond, msg)                                             \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* L1.1 — single-thread acquire / release                            */
/* ------------------------------------------------------------------ */
static void test_single_thread_acquire_release(void)
{
    struct ticket_lock l;
    ticket_lock_init(&l);

    ticket_lock_lock(&l);
    ticket_lock_unlock(&l);

    /* Subsequent acquire must also succeed. */
    ticket_lock_lock(&l);
    ticket_lock_unlock(&l);

    TEST_ASSERT(atomic_load(&l.ticket) == 2,
                "two acquires → ticket == 2");
    TEST_ASSERT(atomic_load(&l.serving) == 2,
                "two unlocks → serving == 2");
}

/* ------------------------------------------------------------------ */
/* L1.2 — try_lock on uncontended lock                                */
/* ------------------------------------------------------------------ */
static void test_try_lock_uncontended(void)
{
    struct ticket_lock l;
    ticket_lock_init(&l);

    bool ok = ticket_lock_try_lock(&l);
    TEST_ASSERT(ok, "try_lock succeeds when lock is free");
    TEST_ASSERT(atomic_load(&l.ticket) == 1, "ticket advanced");
    /* Lock is held; serving stays at 0 until unlock. */
    TEST_ASSERT(atomic_load(&l.serving) == 0, "serving == 0 while lock held");

    ticket_lock_unlock(&l);
    TEST_ASSERT(atomic_load(&l.serving) == 1, "serving advanced after unlock");
}

/* ------------------------------------------------------------------ */
/* L1.3 — try_lock fails when held                                    */
/* ------------------------------------------------------------------ */
static void test_try_lock_contended(void)
{
    struct ticket_lock l;
    ticket_lock_init(&l);

    ticket_lock_lock(&l);
    bool ok = ticket_lock_try_lock(&l);
    TEST_ASSERT(!ok, "try_lock fails when lock is held");
    ticket_lock_unlock(&l);
}

/* ------------------------------------------------------------------ */
/* FIFO acquisition order                                             */
/*                                                                     */
/* Main holds the lock while workers are created one-by-one. Each      */
/* worker signals before calling ticket_lock_lock, so the main thread  */
/* serializes creation — guaranteeing ticket-issue order matches       */
/* creation order 0, 1, …, N-1. After the main thread releases,        */
/* workers record their id in acquisition order, which must be         */
/* exactly 0, 1, …, N-1 for a FIFO-fair lock.                         */
/* ------------------------------------------------------------------ */
#define FIFO_N 8

struct fifo_ctx {
    struct ticket_lock *lock;
    _Atomic int         started[FIFO_N];
    int                 order[FIFO_N];
    _Atomic int         next_slot;
};

struct fifo_worker_arg {
    struct fifo_ctx *ctx;
    int              id;
};

static void *fifo_worker(void *arg)
{
    struct fifo_worker_arg *w = (struct fifo_worker_arg *)arg;
    struct fifo_ctx *ctx = w->ctx;
    int id = w->id;

    /* Signal that we are about to issue a ticket. */
    atomic_store(&ctx->started[id], 1);
    ticket_lock_lock(ctx->lock);

    int slot = atomic_fetch_add(&ctx->next_slot, 1);
    ctx->order[slot] = id;

    ticket_lock_unlock(ctx->lock);
    return NULL;
}

static void test_fifo_order(void)
{
    struct ticket_lock l;
    ticket_lock_init(&l);

    /* Main holds the lock so workers queue behind it. */
    ticket_lock_lock(&l);

    struct fifo_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.lock = &l;

    pthread_t tids[FIFO_N];
    struct fifo_worker_arg args[FIFO_N];

    for (int i = 0; i < FIFO_N; i++) {
        args[i].ctx = &ctx;
        args[i].id = i;
        pthread_create(&tids[i], NULL, fifo_worker, &args[i]);

        /* Wait until worker i has set started[i], ensuring its
         * ticket_lock_lock call (and thus ticket issue) has begun. */
        while (atomic_load(&ctx.started[i]) == 0) {
            sched_yield();
        }
    }

    /* Give the last worker a moment to reach the spin inside
     * ticket_lock_lock, then release. Workers now acquire in the
     * order they issued tickets: 0, 1, …, FIFO_N-1. */
    ticket_lock_unlock(&l);

    for (int i = 0; i < FIFO_N; i++) {
        pthread_join(tids[i], NULL);
    }

    for (int i = 0; i < FIFO_N; i++) {
        TEST_ASSERT(ctx.order[i] == i, "FIFO: order[i] == i");
    }
    TEST_ASSERT(atomic_load(&l.ticket) == FIFO_N + 1,
                "ticket == N + 1 (N workers + main)");
    TEST_ASSERT(atomic_load(&l.serving) == FIFO_N + 1,
                "serving == N + 1");
}

/* ------------------------------------------------------------------ */
/* L1.5 — 64 threads × 10000 iterations stress                        */
/* ------------------------------------------------------------------ */
#define STRESS_NTHREADS 64
#define STRESS_ITERS    10000

struct stress_ctx {
    struct ticket_lock *lock;
    u64                *counter;
    int                 iterations;
};

static void *stress_worker(void *arg)
{
    struct stress_ctx *ctx = (struct stress_ctx *)arg;
    for (int i = 0; i < ctx->iterations; i++) {
        ticket_lock_lock(ctx->lock);
        (*ctx->counter)++;
        ticket_lock_unlock(ctx->lock);
    }
    return NULL;
}

static void test_stress_64x10000(void)
{
    struct ticket_lock l;
    ticket_lock_init(&l);

    u64 counter = 0;
    struct stress_ctx ctx;
    ctx.lock = &l;
    ctx.counter = &counter;
    ctx.iterations = STRESS_ITERS;

    pthread_t tids[STRESS_NTHREADS];
    for (int i = 0; i < STRESS_NTHREADS; i++) {
        pthread_create(&tids[i], NULL, stress_worker, &ctx);
    }
    for (int i = 0; i < STRESS_NTHREADS; i++) {
        pthread_join(tids[i], NULL);
    }

    TEST_ASSERT(counter == (u64)STRESS_NTHREADS * STRESS_ITERS,
                "counter == NTHREADS * ITERS (no lost increments)");
    TEST_ASSERT(atomic_load(&l.ticket) == (u64)STRESS_NTHREADS * STRESS_ITERS,
                "ticket matches total operations");
    TEST_ASSERT(atomic_load(&l.serving) == (u64)STRESS_NTHREADS * STRESS_ITERS,
                "serving matches total operations");
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("test_ticket_lock: L1.1 single-thread ... ");
    test_single_thread_acquire_release();
    printf("PASS\n");

    printf("test_ticket_lock: L1.2 try_lock uncontended ... ");
    test_try_lock_uncontended();
    printf("PASS\n");

    printf("test_ticket_lock: L1.3 try_lock contended ... ");
    test_try_lock_contended();
    printf("PASS\n");

    printf("test_ticket_lock: L1.4 FIFO order ... ");
    test_fifo_order();
    printf("PASS\n");

    printf("test_ticket_lock: L1.5 stress 64×10000 ... ");
    test_stress_64x10000();
    printf("PASS\n");

    printf("test_ticket_lock: ALL PASS\n");
    return 0;
}
