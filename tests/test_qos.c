#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "controller/qos.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        tests_failed++; \
    } \
} while(0)

static void print_separator(void)
{
    printf("========================================\n");
}

/* ---- DWRR scheduler tests ---- */

static void test_dwrr_init_cleanup(void)
{
    printf("\n=== DWRR Init/Cleanup ===\n");

    struct dwrr_scheduler sched;
    int ret;

    ret = dwrr_init(&sched, 128);
    TEST_ASSERT(ret == HFSSS_OK, "dwrr_init succeeds");
    TEST_ASSERT(sched.initialized == true, "scheduler marked initialized");
    TEST_ASSERT(sched.max_outstanding == 128, "max_outstanding stored");
    TEST_ASSERT(sched.throttle_factor == 1.0, "default throttle factor is 1.0");
    TEST_ASSERT(sched.active_count == 0, "no active queues initially");

    dwrr_cleanup(&sched);
    TEST_ASSERT(sched.initialized == false, "cleanup clears initialized flag");

    /* NULL handling */
    ret = dwrr_init(NULL, 128);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "dwrr_init NULL returns INVAL");
    dwrr_cleanup(NULL);  /* should not crash */
    TEST_ASSERT(true, "dwrr_cleanup NULL does not crash");
}

static void test_dwrr_queue_create_delete(void)
{
    printf("\n=== DWRR Queue Create/Delete ===\n");

    struct dwrr_scheduler sched;
    int ret;

    dwrr_init(&sched, 128);

    ret = dwrr_queue_create(&sched, 1, 100);
    TEST_ASSERT(ret == HFSSS_OK, "create queue nsid=1 succeeds");
    TEST_ASSERT(sched.active_count == 1, "active_count is 1");

    ret = dwrr_queue_create(&sched, 2, 200);
    TEST_ASSERT(ret == HFSSS_OK, "create queue nsid=2 succeeds");
    TEST_ASSERT(sched.active_count == 2, "active_count is 2");

    /* Duplicate nsid */
    ret = dwrr_queue_create(&sched, 1, 100);
    TEST_ASSERT(ret == HFSSS_ERR_EXIST, "duplicate nsid returns EXIST");

    /* Delete */
    ret = dwrr_queue_delete(&sched, 1);
    TEST_ASSERT(ret == HFSSS_OK, "delete queue nsid=1 succeeds");
    TEST_ASSERT(sched.active_count == 1, "active_count is 1 after delete");

    /* Delete non-existent */
    ret = dwrr_queue_delete(&sched, 99);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "delete non-existent returns NOENT");

    /* Default weight when 0 */
    ret = dwrr_queue_create(&sched, 3, 0);
    TEST_ASSERT(ret == HFSSS_OK, "create queue with weight=0 uses default");

    dwrr_cleanup(&sched);
}

static void test_dwrr_equal_weight_fairness(void)
{
    printf("\n=== DWRR Equal Weight Fairness ===\n");

    struct dwrr_scheduler sched;
    u32 nsid_out;
    int ret;

    dwrr_init(&sched, 128);
    dwrr_queue_create(&sched, 1, 100);
    dwrr_queue_create(&sched, 2, 100);

    /* Enqueue 100 commands to each */
    for (int i = 0; i < 100; i++) {
        dwrr_enqueue(&sched, 1);
        dwrr_enqueue(&sched, 2);
    }

    TEST_ASSERT(dwrr_has_pending(&sched), "has pending after enqueue");

    u32 count_ns1 = 0, count_ns2 = 0;
    for (int i = 0; i < 200; i++) {
        ret = dwrr_dequeue(&sched, &nsid_out);
        if (ret == HFSSS_OK) {
            if (nsid_out == 1) count_ns1++;
            else if (nsid_out == 2) count_ns2++;
        }
    }

    TEST_ASSERT(count_ns1 + count_ns2 == 200,
                "all 200 commands dispatched");

    /* With equal weights, dispatch should be balanced within 10% */
    int diff = (int)count_ns1 - (int)count_ns2;
    if (diff < 0) diff = -diff;
    TEST_ASSERT(diff <= 20, "equal weight dispatch is balanced (within 10%)");

    TEST_ASSERT(!dwrr_has_pending(&sched), "no pending after all dispatched");

    dwrr_cleanup(&sched);
}

static void test_dwrr_unequal_weight(void)
{
    printf("\n=== DWRR Unequal Weight (3:1 ratio) ===\n");

    struct dwrr_scheduler sched;
    u32 nsid_out;
    int ret;

    dwrr_init(&sched, 128);
    /* Use small weights so deficit rounds are short */
    dwrr_queue_create(&sched, 1, 3);  /* weight 3 */
    dwrr_queue_create(&sched, 2, 1);  /* weight 1 */

    /* Enqueue enough commands so neither queue runs out of supply.
     * base_quantum=10, so NS1 deficit=30, NS2 deficit=10 per round.
     * Over 4000 dispatches we see clear proportional distribution. */
    for (int i = 0; i < 4000; i++) {
        dwrr_enqueue(&sched, 1);
    }
    for (int i = 0; i < 4000; i++) {
        dwrr_enqueue(&sched, 2);
    }

    u32 count_ns1 = 0, count_ns2 = 0;
    u32 total_dispatched = 0;

    for (int i = 0; i < 4000; i++) {
        ret = dwrr_dequeue(&sched, &nsid_out);
        if (ret == HFSSS_OK) {
            if (nsid_out == 1) count_ns1++;
            else if (nsid_out == 2) count_ns2++;
            total_dispatched++;
        }
    }

    TEST_ASSERT(total_dispatched == 4000, "4000 commands dispatched");

    /* Expect ~3:1 ratio within 10% tolerance */
    double ratio = (count_ns2 > 0) ? (double)count_ns1 / count_ns2 : 0;
    TEST_ASSERT(ratio >= 2.7 && ratio <= 3.3,
                "3:1 weight ratio gives proportional dispatch (+/-10%)");

    printf("    nsid=1 dispatched: %u, nsid=2 dispatched: %u, ratio: %.2f\n",
           count_ns1, count_ns2, ratio);

    dwrr_cleanup(&sched);
}

static void test_dwrr_throttle_factor(void)
{
    printf("\n=== DWRR Throttle Factor ===\n");

    struct dwrr_scheduler sched;
    u32 nsid_out;

    dwrr_init(&sched, 128);
    dwrr_queue_create(&sched, 1, 100);

    /* Set throttle factor to 0.5 */
    dwrr_set_throttle_factor(&sched, 0.5);
    TEST_ASSERT(sched.throttle_factor == 0.5, "throttle factor set to 0.5");

    /* Enqueue and dequeue to verify it still works */
    dwrr_enqueue(&sched, 1);
    int ret = dwrr_dequeue(&sched, &nsid_out);
    TEST_ASSERT(ret == HFSSS_OK, "dequeue works with throttle factor 0.5");

    /* Boundary: factor clamped to [0.0, 1.0] */
    dwrr_set_throttle_factor(&sched, -0.5);
    TEST_ASSERT(sched.throttle_factor == 0.0, "negative factor clamped to 0.0");

    dwrr_set_throttle_factor(&sched, 1.5);
    TEST_ASSERT(sched.throttle_factor == 1.0, "factor > 1.0 clamped to 1.0");

    dwrr_cleanup(&sched);
}

static void test_dwrr_get_stats(void)
{
    printf("\n=== DWRR Get Stats ===\n");

    struct dwrr_scheduler sched;
    u32 pending, dispatched, nsid_out;

    dwrr_init(&sched, 128);
    dwrr_queue_create(&sched, 1, 100);

    dwrr_get_stats(&sched, 1, &pending, &dispatched);
    TEST_ASSERT(pending == 0, "initial pending is 0");
    TEST_ASSERT(dispatched == 0, "initial dispatched is 0");

    dwrr_enqueue(&sched, 1);
    dwrr_enqueue(&sched, 1);
    dwrr_get_stats(&sched, 1, &pending, &dispatched);
    TEST_ASSERT(pending == 2, "pending is 2 after 2 enqueues");

    dwrr_dequeue(&sched, &nsid_out);
    dwrr_get_stats(&sched, 1, &pending, &dispatched);
    TEST_ASSERT(pending == 1, "pending is 1 after 1 dequeue");
    TEST_ASSERT(dispatched == 1, "dispatched is 1 after 1 dequeue");

    /* Non-existent nsid */
    dwrr_get_stats(&sched, 99, &pending, &dispatched);
    TEST_ASSERT(pending == 0, "non-existent nsid returns pending=0");

    dwrr_cleanup(&sched);
}

static void test_dwrr_command_cost(void)
{
    printf("\n=== DWRR Command Cost ===\n");

    TEST_ASSERT(dwrr_command_cost(false) == 1, "read cost is 1");
    TEST_ASSERT(dwrr_command_cost(true) == 2, "write cost is 2");
}

/* ---- Token bucket / QoS policy tests ---- */

static void test_qos_iops_limiting(void)
{
    printf("\n=== QoS IOPS Limiting ===\n");

    struct ns_qos_ctx ctx;
    struct ns_qos_policy policy;
    int ret;

    memset(&policy, 0, sizeof(policy));
    policy.nsid = 1;
    policy.iops_limit = 10;
    policy.bw_limit_mbps = 0;
    policy.burst_allowance = 0;
    policy.enforced = true;

    ret = qos_ctx_init(&ctx, 1, &policy);
    TEST_ASSERT(ret == HFSSS_OK, "qos_ctx_init succeeds");
    TEST_ASSERT(ctx.initialized, "qos ctx is initialized");

    /* Should allow up to 10 IOPS (reads cost 1 each) */
    int allowed = 0;
    for (int i = 0; i < 20; i++) {
        if (qos_acquire_tokens(&ctx, false, 4096)) {
            allowed++;
        }
    }
    TEST_ASSERT(allowed == 10, "IOPS limit allows exactly 10 reads");

    /* After exhaustion, should deny */
    TEST_ASSERT(!qos_acquire_tokens(&ctx, false, 4096),
                "acquire fails when tokens exhausted");

    qos_ctx_cleanup(&ctx);
    TEST_ASSERT(!ctx.initialized, "cleanup clears initialized");
}

static void test_qos_bw_limiting(void)
{
    printf("\n=== QoS BW Limiting ===\n");

    struct ns_qos_ctx ctx;
    struct ns_qos_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.nsid = 1;
    policy.iops_limit = 0;       /* unlimited IOPS */
    policy.bw_limit_mbps = 1;    /* 1 MB/s = 1048576 bytes/s */
    policy.burst_allowance = 0;
    policy.enforced = true;

    qos_ctx_init(&ctx, 1, &policy);

    /* Each IO is 4096 bytes. 1MB = 1048576 bytes, so 256 IOs */
    int allowed = 0;
    for (int i = 0; i < 300; i++) {
        if (qos_acquire_tokens(&ctx, false, 4096)) {
            allowed++;
        }
    }
    TEST_ASSERT(allowed == 256, "BW limit allows exactly 256 x 4KB reads (1MB)");

    qos_ctx_cleanup(&ctx);
}

static void test_qos_refill(void)
{
    printf("\n=== QoS Token Refill ===\n");

    struct ns_qos_ctx ctx;
    struct ns_qos_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.nsid = 1;
    policy.iops_limit = 1000;
    policy.bw_limit_mbps = 0;
    policy.burst_allowance = 0;
    policy.enforced = true;

    qos_ctx_init(&ctx, 1, &policy);

    /* Drain all tokens */
    while (qos_acquire_tokens(&ctx, false, 4096)) {
        /* drain */
    }
    TEST_ASSERT(!qos_acquire_tokens(&ctx, false, 4096),
                "tokens exhausted after drain");

    /* Simulate time passing: 1 second at 1000 tokens/sec */
    u64 future_ns = get_time_ns() + 1000000000ULL;
    qos_refill_tokens(&ctx, future_ns);

    /* Should have tokens again */
    TEST_ASSERT(qos_acquire_tokens(&ctx, false, 4096),
                "tokens available after refill");

    qos_ctx_cleanup(&ctx);
}

static void test_qos_hot_reconfigure(void)
{
    printf("\n=== QoS Hot Reconfiguration ===\n");

    struct ns_qos_ctx ctx;
    struct ns_qos_policy policy;
    struct ns_qos_policy readback;

    memset(&policy, 0, sizeof(policy));
    policy.nsid = 1;
    policy.iops_limit = 100;
    policy.enforced = true;

    qos_ctx_init(&ctx, 1, &policy);

    qos_get_policy(&ctx, &readback);
    TEST_ASSERT(readback.iops_limit == 100, "initial IOPS limit is 100");

    /* Change to 50 */
    policy.iops_limit = 50;
    int ret = qos_set_policy(&ctx, &policy);
    TEST_ASSERT(ret == HFSSS_OK, "qos_set_policy succeeds");

    qos_get_policy(&ctx, &readback);
    TEST_ASSERT(readback.iops_limit == 50, "IOPS limit changed to 50");

    /* Verify the new limit takes effect */
    int allowed = 0;
    for (int i = 0; i < 100; i++) {
        if (qos_acquire_tokens(&ctx, false, 4096)) {
            allowed++;
        }
    }
    TEST_ASSERT(allowed == 50, "new IOPS limit of 50 enforced");

    qos_ctx_cleanup(&ctx);
}

static void test_qos_unenforced(void)
{
    printf("\n=== QoS Unenforced Policy ===\n");

    struct ns_qos_ctx ctx;
    struct ns_qos_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.nsid = 1;
    policy.iops_limit = 5;
    policy.enforced = false;  /* not enforced */

    qos_ctx_init(&ctx, 1, &policy);

    /* All should pass since not enforced */
    int allowed = 0;
    for (int i = 0; i < 100; i++) {
        if (qos_acquire_tokens(&ctx, false, 4096)) {
            allowed++;
        }
    }
    TEST_ASSERT(allowed == 100, "unenforced policy allows all IOs");

    qos_ctx_cleanup(&ctx);
}

/* ---- Latency monitor tests ---- */

static void test_latency_monitor_histogram(void)
{
    printf("\n=== Latency Monitor Histogram ===\n");

    struct ns_latency_monitor mon;
    int ret;

    ret = lat_monitor_init(&mon, 1, 1000);
    TEST_ASSERT(ret == HFSSS_OK, "lat_monitor_init succeeds");
    TEST_ASSERT(mon.initialized, "monitor is initialized");

    /* Record some latencies (in nanoseconds):
     * 100us = 100000ns -> bucket ~6 (64-128us range)
     * 500us = 500000ns -> bucket ~8 (256-512us range)
     * 1000us = 1000000ns -> bucket ~9 (512-1024us range) */
    for (int i = 0; i < 90; i++) {
        lat_monitor_record(&mon, 100000);  /* 100us */
    }
    for (int i = 0; i < 9; i++) {
        lat_monitor_record(&mon, 500000);  /* 500us */
    }
    lat_monitor_record(&mon, 1000000);  /* 1000us = 1ms */

    TEST_ASSERT(mon.total_samples == 100, "total samples is 100");

    /* P50 should be around 100us range */
    u64 p50 = lat_monitor_percentile(&mon, 500);
    TEST_ASSERT(p50 <= 256, "P50 is in the 100us bucket range");

    /* P99 should capture the 500us sample */
    u64 p99 = lat_monitor_percentile(&mon, 990);
    TEST_ASSERT(p99 >= 256 && p99 <= 2048, "P99 reflects 500us-1ms range");

    /* P99.9 should capture the 1ms outlier */
    u64 p999 = lat_monitor_percentile(&mon, 999);
    TEST_ASSERT(p999 >= 512, "P99.9 captures the 1ms outlier");

    lat_monitor_cleanup(&mon);
}

static void test_latency_sla_violation(void)
{
    printf("\n=== Latency SLA Violation Detection ===\n");

    struct ns_latency_monitor mon;

    /* Target: P99 < 200us */
    lat_monitor_init(&mon, 1, 200);

    /* Record latencies all below target */
    for (int i = 0; i < 100; i++) {
        lat_monitor_record(&mon, 50000);  /* 50us */
    }

    bool violated = lat_monitor_check_sla(&mon);
    TEST_ASSERT(!violated, "no SLA violation when all latencies below target");
    TEST_ASSERT(mon.sla_violations == 0, "violation count is 0");

    /* Reset and add violations */
    lat_monitor_reset(&mon);
    TEST_ASSERT(mon.total_samples == 0, "reset clears samples");

    /* Record: 95 at 50us, 5 at 500us -> P99 will be in 500us range */
    for (int i = 0; i < 95; i++) {
        lat_monitor_record(&mon, 50000);  /* 50us */
    }
    for (int i = 0; i < 5; i++) {
        lat_monitor_record(&mon, 500000);  /* 500us */
    }

    violated = lat_monitor_check_sla(&mon);
    TEST_ASSERT(violated, "SLA violation detected when P99 exceeds target");
    TEST_ASSERT(mon.sla_violations == 1, "violation count incremented");
    TEST_ASSERT(mon.consecutive_violations == 1, "consecutive violations is 1");

    /* Check again (still violated) */
    violated = lat_monitor_check_sla(&mon);
    TEST_ASSERT(violated, "SLA still violated on second check");
    TEST_ASSERT(mon.consecutive_violations == 2, "consecutive violations is 2");

    lat_monitor_cleanup(&mon);
}

static void test_latency_monitor_null(void)
{
    printf("\n=== Latency Monitor NULL Handling ===\n");

    TEST_ASSERT(lat_monitor_init(NULL, 1, 100) == HFSSS_ERR_INVAL,
                "init NULL returns INVAL");

    lat_monitor_cleanup(NULL);
    TEST_ASSERT(true, "cleanup NULL does not crash");

    lat_monitor_record(NULL, 1000);
    TEST_ASSERT(true, "record NULL does not crash");

    u64 p = lat_monitor_percentile(NULL, 990);
    TEST_ASSERT(p == 0, "percentile NULL returns 0");
}

/* ---- Deterministic window tests ---- */

static void test_det_window_phase(void)
{
    printf("\n=== Deterministic Window Phase Calculation ===\n");

    struct det_window_config cfg;
    int ret;

    ret = det_window_init(&cfg, 80, 15, 5, 100);
    TEST_ASSERT(ret == HFSSS_OK, "det_window_init succeeds");
    TEST_ASSERT(cfg.enabled, "window is enabled");

    u64 start = cfg.cycle_start_ns;
    u64 cycle_ns = 100ULL * 1000000ULL;  /* 100ms */

    /* At 0% of cycle -> HOST_IO */
    enum det_window_phase phase = det_window_get_phase(&cfg, start);
    TEST_ASSERT(phase == DW_HOST_IO, "phase at 0% is HOST_IO");

    /* At 40% of cycle -> HOST_IO (within 80%) */
    phase = det_window_get_phase(&cfg, start + cycle_ns * 40 / 100);
    TEST_ASSERT(phase == DW_HOST_IO, "phase at 40% is HOST_IO");

    /* At 79% of cycle -> HOST_IO */
    phase = det_window_get_phase(&cfg, start + cycle_ns * 79 / 100);
    TEST_ASSERT(phase == DW_HOST_IO, "phase at 79% is HOST_IO");

    /* At 85% of cycle -> GC_ALLOWED (80-95%) */
    phase = det_window_get_phase(&cfg, start + cycle_ns * 85 / 100);
    TEST_ASSERT(phase == DW_GC_ALLOWED, "phase at 85% is GC_ALLOWED");

    /* At 96% of cycle -> GC_ONLY (95-100%) */
    phase = det_window_get_phase(&cfg, start + cycle_ns * 96 / 100);
    TEST_ASSERT(phase == DW_GC_ONLY, "phase at 96% is GC_ONLY");

    /* Wrap around: at 100% + 10% of next cycle -> HOST_IO */
    phase = det_window_get_phase(&cfg, start + cycle_ns + cycle_ns * 10 / 100);
    TEST_ASSERT(phase == DW_HOST_IO, "phase wraps around to HOST_IO");
}

static void test_det_window_permissions(void)
{
    printf("\n=== Deterministic Window GC/Host Permissions ===\n");

    struct det_window_config cfg;

    det_window_init(&cfg, 80, 15, 5, 100);

    u64 start = cfg.cycle_start_ns;
    u64 cycle_ns = 100ULL * 1000000ULL;

    /* HOST_IO phase: host=yes, gc=no */
    u64 t_host = start + cycle_ns * 10 / 100;
    TEST_ASSERT(det_window_allow_host_io(&cfg, t_host),
                "host IO allowed in HOST_IO phase");
    TEST_ASSERT(!det_window_allow_gc(&cfg, t_host),
                "GC not allowed in HOST_IO phase");

    /* GC_ALLOWED phase: host=yes, gc=yes */
    u64 t_gc_allowed = start + cycle_ns * 85 / 100;
    TEST_ASSERT(det_window_allow_host_io(&cfg, t_gc_allowed),
                "host IO allowed in GC_ALLOWED phase");
    TEST_ASSERT(det_window_allow_gc(&cfg, t_gc_allowed),
                "GC allowed in GC_ALLOWED phase");

    /* GC_ONLY phase: host=no, gc=yes */
    u64 t_gc_only = start + cycle_ns * 97 / 100;
    TEST_ASSERT(!det_window_allow_host_io(&cfg, t_gc_only),
                "host IO not allowed in GC_ONLY phase");
    TEST_ASSERT(det_window_allow_gc(&cfg, t_gc_only),
                "GC allowed in GC_ONLY phase");
}

static void test_det_window_invalid(void)
{
    printf("\n=== Deterministic Window Invalid Config ===\n");

    struct det_window_config cfg;
    int ret;

    /* Percentages don't add up to 100 */
    ret = det_window_init(&cfg, 80, 15, 10, 100);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "reject when pct sum != 100");

    /* Zero cycle */
    ret = det_window_init(&cfg, 80, 15, 5, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "reject zero cycle_ms");

    /* NULL */
    ret = det_window_init(NULL, 80, 15, 5, 100);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "reject NULL config");

    /* NULL allow checks default to true */
    TEST_ASSERT(det_window_allow_gc(NULL, 0), "NULL config allows GC");
    TEST_ASSERT(det_window_allow_host_io(NULL, 0), "NULL config allows host IO");
}

/* REQ-088: P99.9 latency anomaly detector. The monitor accumulates
 * histogram samples via the existing lat_monitor_record API; the
 * anomaly check computes current P99.9 and, if it exceeds the
 * configured threshold, bumps a counter and optionally fires the
 * caller-supplied alert callback. */
static u32 g_p999_alert_calls = 0;
static u64 g_p999_last_reading = 0;
static u32 g_p999_last_nsid    = 0;
static void p999_alert_cb(u32 nsid, u64 p999_us, void *ctx)
{
    (void)ctx;
    g_p999_alert_calls++;
    g_p999_last_reading = p999_us;
    g_p999_last_nsid    = nsid;
}

static void test_latency_p999_anomaly_detector(void)
{
    print_separator();
    printf("Latency Monitor P99.9 anomaly detector (REQ-088)\n");
    print_separator();

    struct ns_latency_monitor mon;
    int ret = lat_monitor_init(&mon, 42, 1000);
    TEST_ASSERT(ret == HFSSS_OK, "p999: lat_monitor_init");

    /* Threshold 4ms: healthy samples stay under it. */
    ret = lat_monitor_set_p999_anomaly(&mon, 4000, p999_alert_cb, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "p999: threshold + cb installed");

    /* 1000 healthy samples around 500us -> P99.9 well below 4ms. */
    g_p999_alert_calls = 0;
    for (int i = 0; i < 1000; i++) {
        lat_monitor_record(&mon, 500000);  /* 500us */
    }
    bool fired = lat_monitor_check_p999_anomaly(&mon);
    TEST_ASSERT(fired == false,
                "p999: healthy workload doesn't fire");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 0,
                "p999: anomaly count stays 0");
    TEST_ASSERT(g_p999_alert_calls == 0,
                "p999: alert callback not fired yet");

    /* Inject a handful of outliers big enough to push P99.9 past
     * the 4ms threshold (4 outliers at 64ms over 1004 samples ~
     * 0.4% -> P99.9 lands in the 64ms bucket). */
    for (int i = 0; i < 4; i++) {
        lat_monitor_record(&mon, 64ULL * 1000 * 1000);  /* 64ms */
    }
    fired = lat_monitor_check_p999_anomaly(&mon);
    TEST_ASSERT(fired == true,
                "p999: outliers push P99.9 above threshold");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 1,
                "p999: anomaly count == 1 after first breach");
    TEST_ASSERT(g_p999_alert_calls == 1,
                "p999: alert callback fired once");
    TEST_ASSERT(g_p999_last_nsid == 42,
                "p999: callback received correct nsid");
    TEST_ASSERT(g_p999_last_reading > 4000,
                "p999: callback received a P99.9 reading above threshold");

    /* Another check (same state) continues to see the anomaly. */
    fired = lat_monitor_check_p999_anomaly(&mon);
    TEST_ASSERT(fired == true,
                "p999: breach persists until histogram is reset");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 2,
                "p999: anomaly count == 2 after repeat check");

    /* Disabling the detector stops further counting, even while the
     * histogram is still degraded. */
    ret = lat_monitor_set_p999_anomaly(&mon, 0, NULL, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "p999: detector disabled");
    fired = lat_monitor_check_p999_anomaly(&mon);
    TEST_ASSERT(fired == false,
                "p999: disabled detector reports no breach");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 2,
                "p999: counter unchanged when disabled");

    /* NULL safety + disabled-via-NULL-cb path. */
    TEST_ASSERT(lat_monitor_set_p999_anomaly(NULL, 1000, NULL, NULL)
                == HFSSS_ERR_INVAL, "p999: NULL monitor rejected");
    TEST_ASSERT(lat_monitor_check_p999_anomaly(NULL) == false,
                "p999: NULL monitor check returns false");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(NULL) == 0,
                "p999: NULL monitor count returns 0");

    lat_monitor_cleanup(&mon);
}

/* REQ-088 PR #95 review guard: lat_monitor_reset must scrub the
 * histogram and sla counters but preserve the P99.9 detector
 * (threshold, anomaly counter, installed callback) so an alert
 * installed at boot survives window-boundary resets. */
static void test_latency_p999_reset_preservation(void)
{
    print_separator();
    printf("Latency Monitor P99.9 reset preservation (REQ-088)\n");
    print_separator();

    struct ns_latency_monitor mon;
    int ret = lat_monitor_init(&mon, 7, 1000);
    TEST_ASSERT(ret == HFSSS_OK, "p999-reset: lat_monitor_init");

    g_p999_alert_calls = 0;
    ret = lat_monitor_set_p999_anomaly(&mon, 4000, p999_alert_cb, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "p999-reset: detector installed");

    /* Record enough outliers to trigger a breach, then reset. */
    for (int i = 0; i < 1000; i++) lat_monitor_record(&mon, 500000);
    for (int i = 0; i < 4;    i++) lat_monitor_record(&mon, 64ULL * 1000 * 1000);
    TEST_ASSERT(lat_monitor_check_p999_anomaly(&mon) == true,
                "p999-reset: baseline breach fires");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 1,
                "p999-reset: counter == 1 pre-reset");

    lat_monitor_reset(&mon);

    /* Histogram gone: check should report no breach from zero
     * samples. But detector config + counter persist. */
    TEST_ASSERT(mon.total_samples == 0,
                "p999-reset: histogram scrubbed");
    TEST_ASSERT(lat_monitor_check_p999_anomaly(&mon) == false,
                "p999-reset: no breach with empty histogram");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 1,
                "p999-reset: anomaly counter preserved across reset");
    TEST_ASSERT(mon.p999_threshold_us == 4000,
                "p999-reset: threshold preserved");
    TEST_ASSERT(mon.p999_cb == p999_alert_cb,
                "p999-reset: callback preserved");

    /* Re-breach after reset still fires the existing callback. */
    u32 before = g_p999_alert_calls;
    for (int i = 0; i < 1000; i++) lat_monitor_record(&mon, 500000);
    for (int i = 0; i < 4;    i++) lat_monitor_record(&mon, 64ULL * 1000 * 1000);
    TEST_ASSERT(lat_monitor_check_p999_anomaly(&mon) == true,
                "p999-reset: post-reset workload breaches again");
    TEST_ASSERT(g_p999_alert_calls == before + 1,
                "p999-reset: preserved callback fired after reset");
    TEST_ASSERT(lat_monitor_p999_anomaly_count(&mon) == 2,
                "p999-reset: counter continues past reset boundary");

    lat_monitor_cleanup(&mon);
}

/* ---- main ---- */

int main(void)
{
    print_separator();
    printf("QoS Determinism Module Tests\n");
    print_separator();

    /* DWRR tests */
    test_dwrr_init_cleanup();
    test_dwrr_queue_create_delete();
    test_dwrr_equal_weight_fairness();
    test_dwrr_unequal_weight();
    test_dwrr_throttle_factor();
    test_dwrr_get_stats();
    test_dwrr_command_cost();

    /* Token bucket / QoS policy tests */
    test_qos_iops_limiting();
    test_qos_bw_limiting();
    test_qos_refill();
    test_qos_hot_reconfigure();
    test_qos_unenforced();

    /* Latency monitor tests */
    test_latency_monitor_histogram();
    test_latency_sla_violation();
    test_latency_monitor_null();
    test_latency_p999_anomaly_detector();
    test_latency_p999_reset_preservation();

    /* Deterministic window tests */
    test_det_window_phase();
    test_det_window_permissions();
    test_det_window_invalid();

    print_separator();
    printf("QoS Tests: %d/%d passed, %d failed\n",
           tests_passed, tests_run, tests_failed);
    print_separator();

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}
