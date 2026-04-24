/*
 * REQ-137: smoke test for the stress_stability ↔ system_monitor wiring.
 *
 * The full burn-in harness runs under the `stress-burn-in` make target
 * for an hour or more. To keep regressions from sneaking in between
 * those runs, this file drives the same initialization sequence that
 * stress_stability.c uses (config → init → start → progress poll →
 * stop → cleanup), asserts the monitor produced at least one sample,
 * and confirms the accessors return non-degenerate values. It runs
 * in well under a second so it is safe for the default `make test`.
 *
 * What this explicitly does NOT test:
 *   - The media / fault-injection loop (covered elsewhere).
 *   - Steady-state peak accuracy over a real multi-hour run (that is
 *     the burn-in job itself; the smoke test can never stand in).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "common/common.h"
#include "common/system_monitor.h"

static int g_pass = 0;
static int g_fail = 0;

#define TEST_ASSERT(cond, msg) do {                           \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; }     \
    else      { printf("  [FAIL] %s  (line %d)\n",            \
                       msg, __LINE__); g_fail++; }            \
} while (0)

static u32 one_thread_cb(void *ctx) { (void)ctx; return 1u; }

/* Busy-wait for at least `ms` milliseconds so the background sampler
 * has time to collect two or three data points at 100 Hz. Using a
 * clock_gettime loop instead of usleep avoids any weirdness where
 * a sleeping thread under-reports CPU usage and confuses the
 * fixed-rate samples.
 *
 * The elapsed computation is in signed ns to avoid an u64 underflow
 * when the loop straddles a 1-second boundary with now.tv_nsec <
 * start.tv_nsec — without the signed intermediate the u64 subtract
 * wraps to ~2^64 and the loop exits on the next iteration, making
 * the caller's "samples >= 2" assertion flaky under load. */
static void burn_cpu_ms(u32 ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile u64 acc = 0;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ns =
            ((int64_t)now.tv_sec  - (int64_t)start.tv_sec)  * 1000000000LL +
            ((int64_t)now.tv_nsec - (int64_t)start.tv_nsec);
        if (elapsed_ns < 0) elapsed_ns = 0;  /* monotonic but belt+suspenders */
        if ((u64)(elapsed_ns / 1000000LL) >= (u64)ms) break;
        for (int i = 0; i < 10000; i++) acc += (u64)i;
    }
    (void)acc;
}

static void test_monitor_lifecycle(void)
{
    printf("\n=== system_monitor lifecycle (burn-in shape) ===\n");

    struct system_monitor m;
    struct system_monitor_config cfg = {
        .poll_interval_ms  = 10,
        .get_cpu_time_ns   = system_monitor_default_cpu_time_ns,
        .get_mem_bytes     = system_monitor_default_mem_bytes,
        .get_thread_count  = one_thread_cb,
        .cb_ctx            = NULL,
    };

    TEST_ASSERT(system_monitor_init(&m, &cfg) == HFSSS_OK,
                "init with default POSIX callbacks");

    TEST_ASSERT(system_monitor_start(&m) == HFSSS_OK,
                "start background sampler");

    /* 300 ms at 10 ms poll interval is ~30 expected samples. We only
     * assert ≥2 so a loaded CI runner can miss most of them without
     * flaking. */
    burn_cpu_ms(300);

    u64 samples = system_monitor_sample_count(&m);
    printf("  samples observed: %" PRIu64 "\n", samples);
    TEST_ASSERT(samples >= 2,
                "monitor produced at least two samples in 300 ms");

    u64 mem = system_monitor_mem_bytes(&m);
    TEST_ASSERT(mem > 0, "mem_bytes accessor returns non-zero");

    /* cpu_pct is allowed to be 0.0 on the first sample (baseline
     * seed) but must be a finite non-negative number. */
    double cpu = system_monitor_cpu_pct(&m);
    TEST_ASSERT(cpu >= 0.0 && cpu < 1000.0,
                "cpu_pct accessor returns a sane value");

    u32 thr = system_monitor_thread_count(&m);
    TEST_ASSERT(thr >= 1, "thread_count accessor returns at least 1");

    system_monitor_stop(&m);
    TEST_ASSERT(1, "stop sampler (no crash, returns cleanly)");

    /* After stop the sample_count is frozen; re-query must return the
     * same value the final pre-stop pull saw. This matters for burn-in
     * where the report is printed *after* stop. */
    u64 frozen = system_monitor_sample_count(&m);
    TEST_ASSERT(frozen >= samples,
                "sample_count monotone across stop boundary");

    system_monitor_cleanup(&m);
    TEST_ASSERT(1, "cleanup (no crash on shut-down monitor)");
}

static void test_init_rejects_null_callbacks(void)
{
    printf("\n=== init rejects missing callbacks ===\n");

    struct system_monitor m;
    struct system_monitor_config cfg = {
        .poll_interval_ms  = 10,
        .get_cpu_time_ns   = system_monitor_default_cpu_time_ns,
        .get_mem_bytes     = system_monitor_default_mem_bytes,
        .get_thread_count  = NULL,   /* intentionally missing */
        .cb_ctx            = NULL,
    };
    int rc = system_monitor_init(&m, &cfg);
    TEST_ASSERT(rc != HFSSS_OK,
                "init fails when thread-count callback is NULL "
                "(stress_stability must provide one)");
}

int main(void)
{
    printf("========================================\n");
    printf("REQ-137 burn-in smoke: monitor integration\n");
    printf("========================================\n");

    test_monitor_lifecycle();
    test_init_rejects_null_callbacks();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");
    return g_fail > 0 ? 1 : 0;
}
