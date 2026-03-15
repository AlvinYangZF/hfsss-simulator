#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef HFSSS_TRACE_TEST_MODE
#  define HFSSS_TRACE_TEST_MODE
#endif
#include "common/rt_services.h"
#include "common/common.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void separator(void) {
    printf("========================================\n");
}

/* ------------------------------------------------------------------
 * CPU affinity helpers
 * ------------------------------------------------------------------ */
static void test_affinity_mask(void) {
    separator();
    printf("Test: affinity mask per role\n");
    separator();

    uint64_t mask;
    int ret = rt_get_affinity_mask(RT_ROLE_IO_FAST, &mask);
    TEST_ASSERT(ret == HFSSS_OK, "get_affinity_mask IO_FAST returns OK");
    TEST_ASSERT(mask == (1ULL << 0), "IO_FAST mapped to CPU 0");

    ret = rt_get_affinity_mask(RT_ROLE_GC, &mask);
    TEST_ASSERT(ret == HFSSS_OK, "get_affinity_mask GC returns OK");
    TEST_ASSERT(mask == (1ULL << 1), "GC mapped to CPU 1");

    ret = rt_get_affinity_mask(RT_ROLE_GENERIC, &mask);
    TEST_ASSERT(ret == HFSSS_OK, "get_affinity_mask GENERIC returns OK");
    TEST_ASSERT(mask == 0, "GENERIC has no pinning (mask = 0)");

    ret = rt_get_affinity_mask(RT_ROLE_COUNT, &mask);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "invalid role returns INVAL");

    ret = rt_get_affinity_mask(RT_ROLE_IO_FAST, NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "NULL mask_out returns INVAL");
}

static void test_pin_self(void) {
    separator();
    printf("Test: rt_pin_self (best-effort, non-fatal)\n");
    separator();

    /* Pin self to GENERIC — should always succeed (no-op) */
    int ret = rt_pin_self(RT_ROLE_GENERIC);
    TEST_ASSERT(ret == HFSSS_OK, "pin_self GENERIC returns OK");

    /* Pin self to IO_FAST — may succeed or fail gracefully on CI */
    ret = rt_pin_self(RT_ROLE_IO_FAST);
    TEST_ASSERT(ret == HFSSS_OK || ret == HFSSS_ERR_IO,
                "pin_self IO_FAST returns OK or ERR_IO (platform dependent)");
}

/* ------------------------------------------------------------------
 * IPC channel
 * ------------------------------------------------------------------ */
static void test_ipc_init_cleanup(void) {
    separator();
    printf("Test: rt_ipc_init / cleanup\n");
    separator();

    struct rt_ipc_channel ch;
    int ret = rt_ipc_init(&ch);
    TEST_ASSERT(ret == HFSSS_OK, "rt_ipc_init returns OK");
    TEST_ASSERT(ch.initialized, "channel initialized");
    TEST_ASSERT(rt_ipc_is_empty(&ch), "channel is empty after init");

    rt_ipc_cleanup(&ch);
    TEST_ASSERT(!ch.initialized, "channel not initialized after cleanup");

    TEST_ASSERT(rt_ipc_init(NULL) == HFSSS_ERR_INVAL, "init(NULL) returns INVAL");
}

static void test_ipc_send_recv(void) {
    separator();
    printf("Test: IPC send/recv round-trip\n");
    separator();

    struct rt_ipc_channel ch;
    rt_ipc_init(&ch);

    struct rt_ipc_msg tx = {.type = 42, .len = 4};
    tx.data[0] = 0xDE; tx.data[1] = 0xAD;
    tx.data[2] = 0xBE; tx.data[3] = 0xEF;

    int ret = rt_ipc_send(&ch, &tx);
    TEST_ASSERT(ret == HFSSS_OK, "send returns OK");
    TEST_ASSERT(!rt_ipc_is_empty(&ch), "channel not empty after send");
    TEST_ASSERT(rt_ipc_pending(&ch) == 1, "pending = 1");

    struct rt_ipc_msg rx;
    ret = rt_ipc_recv_timeout(&ch, &rx, 100);
    TEST_ASSERT(ret == HFSSS_OK, "recv_timeout returns OK");
    TEST_ASSERT(rx.type == 42, "rx.type matches tx.type");
    TEST_ASSERT(rx.data[0] == 0xDE, "rx.data[0] matches");
    TEST_ASSERT(rt_ipc_is_empty(&ch), "channel empty after recv");

    rt_ipc_cleanup(&ch);
}

static void test_ipc_full(void) {
    separator();
    printf("Test: IPC channel full returns NOSPC\n");
    separator();

    struct rt_ipc_channel ch;
    rt_ipc_init(&ch);

    struct rt_ipc_msg msg = {.type = 1, .len = 0};
    /* Fill to capacity (RT_IPC_CHANNEL_DEPTH - 1 messages) */
    int ret = HFSSS_OK;
    uint32_t sent = 0;
    while (ret == HFSSS_OK) {
        ret = rt_ipc_send(&ch, &msg);
        if (ret == HFSSS_OK) sent++;
    }
    TEST_ASSERT(ret == HFSSS_ERR_NOSPC, "send to full channel returns NOSPC");
    TEST_ASSERT(sent == RT_IPC_CHANNEL_DEPTH - 1, "sent exactly DEPTH-1 messages");

    rt_ipc_cleanup(&ch);
}

static void test_ipc_timeout(void) {
    separator();
    printf("Test: IPC recv_timeout on empty channel\n");
    separator();

    struct rt_ipc_channel ch;
    rt_ipc_init(&ch);

    struct rt_ipc_msg rx;
    int ret = rt_ipc_recv_timeout(&ch, &rx, 50);  /* 50 ms timeout */
    TEST_ASSERT(ret == HFSSS_ERR_TIMEOUT, "recv_timeout on empty returns TIMEOUT");

    rt_ipc_cleanup(&ch);
}

static void test_ipc_null_safety(void) {
    separator();
    printf("Test: IPC NULL safety\n");
    separator();

    struct rt_ipc_msg msg = {0};
    TEST_ASSERT(rt_ipc_send(NULL, &msg) == HFSSS_ERR_INVAL, "send(NULL ch) returns INVAL");
    TEST_ASSERT(rt_ipc_recv_timeout(NULL, &msg, 0) == HFSSS_ERR_INVAL, "recv(NULL ch) returns INVAL");
    TEST_ASSERT(rt_ipc_is_empty(NULL), "is_empty(NULL) returns true");
    TEST_ASSERT(rt_ipc_pending(NULL) == 0, "pending(NULL) returns 0");
}

/* ------------------------------------------------------------------
 * Trace ring
 * ------------------------------------------------------------------ */
static void test_trace_init_cleanup(void) {
    separator();
    printf("Test: trace_ring_init / cleanup\n");
    separator();

    struct trace_ring ring;
    int ret = trace_ring_init(&ring);
    TEST_ASSERT(ret == HFSSS_OK, "trace_ring_init returns OK");
    TEST_ASSERT(ring.initialized, "ring initialized");
    TEST_ASSERT(trace_ring_pending(&ring) == 0, "no pending entries after init");

    trace_ring_cleanup(&ring);
    TEST_ASSERT(!ring.initialized, "ring not initialized after cleanup");
}

static void test_trace_write_read(void) {
    separator();
    printf("Test: trace write/read round-trip\n");
    separator();

    struct trace_ring ring;
    trace_ring_init(&ring);

    int ret = trace_ring_write(&ring, TRACE_LEVEL_INFO, 1, "hello trace");
    TEST_ASSERT(ret == HFSSS_OK, "trace_ring_write returns OK");
    TEST_ASSERT(trace_ring_pending(&ring) == 1, "pending = 1 after write");

    struct trace_entry e;
    ret = trace_ring_read(&ring, &e);
    TEST_ASSERT(ret == HFSSS_OK, "trace_ring_read returns OK");
    TEST_ASSERT(e.level == TRACE_LEVEL_INFO, "level matches");
    TEST_ASSERT(e.subsystem == 1, "subsystem matches");
    TEST_ASSERT(strncmp(e.msg, "hello trace", 11) == 0, "message matches");
    TEST_ASSERT(e.timestamp_ns > 0, "timestamp set");
    TEST_ASSERT(trace_ring_pending(&ring) == 0, "pending = 0 after read");

    trace_ring_cleanup(&ring);
}

static void test_trace_empty_read(void) {
    separator();
    printf("Test: trace_ring_read on empty ring returns NOENT\n");
    separator();

    struct trace_ring ring;
    trace_ring_init(&ring);

    struct trace_entry e;
    int ret = trace_ring_read(&ring, &e);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "read on empty ring returns NOENT");

    trace_ring_cleanup(&ring);
}

static void test_trace_multiple_entries(void) {
    separator();
    printf("Test: multiple trace entries in sequence\n");
    separator();

    struct trace_ring ring;
    trace_ring_init(&ring);

    for (int i = 0; i < 10; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "entry %d", i);
        trace_ring_write(&ring, TRACE_LEVEL_DEBUG, 0, msg);
    }
    TEST_ASSERT(trace_ring_pending(&ring) == 10, "pending = 10 after 10 writes");

    /* Read all 10 back */
    bool all_ok = true;
    for (int i = 0; i < 10; i++) {
        struct trace_entry e;
        if (trace_ring_read(&ring, &e) != HFSSS_OK) { all_ok = false; break; }
    }
    TEST_ASSERT(all_ok, "all 10 entries read successfully");
    TEST_ASSERT(trace_ring_pending(&ring) == 0, "pending = 0 after reading all");

    trace_ring_cleanup(&ring);
}

static void test_trace_null_safety(void) {
    separator();
    printf("Test: trace ring NULL safety\n");
    separator();

    TEST_ASSERT(trace_ring_init(NULL) == HFSSS_ERR_INVAL, "init(NULL) returns INVAL");
    TEST_ASSERT(trace_ring_write(NULL, TRACE_LEVEL_INFO, 0, "x") == HFSSS_ERR_INVAL,
                "write(NULL ring) returns INVAL");

    struct trace_ring ring;
    trace_ring_init(&ring);
    TEST_ASSERT(trace_ring_write(&ring, TRACE_LEVEL_INFO, 0, NULL) == HFSSS_ERR_INVAL,
                "write(NULL msg) returns INVAL");
    TEST_ASSERT(trace_ring_read(&ring, NULL) == HFSSS_ERR_INVAL,
                "read(NULL out) returns INVAL");

    trace_ring_cleanup(&ring);
}

/* ------------------------------------------------------------------
 * Resource monitor
 * ------------------------------------------------------------------ */
static void test_mon_init_cleanup(void) {
    separator();
    printf("Test: rt_mon_init / cleanup\n");
    separator();

    struct rt_resource_monitor mon;
    int ret = rt_mon_init(&mon, 80, 1000);
    TEST_ASSERT(ret == HFSSS_OK, "rt_mon_init returns OK");
    TEST_ASSERT(mon.initialized, "monitor initialized");
    TEST_ASSERT(mon.cpu_warn_pct == 80, "cpu_warn_pct set");
    TEST_ASSERT(mon.latency_warn_us == 1000, "latency_warn_us set");

    rt_mon_cleanup(&mon);
    TEST_ASSERT(!mon.initialized, "monitor not initialized after cleanup");

    TEST_ASSERT(rt_mon_init(NULL, 0, 0) == HFSSS_ERR_INVAL, "init(NULL) returns INVAL");
}

static void test_mon_record_get(void) {
    separator();
    printf("Test: resource monitor record / get_latest\n");
    separator();

    struct rt_resource_monitor mon;
    rt_mon_init(&mon, 90, 5000);

    struct rt_resource_sample s1 = {0};
    /* No sample yet */
    struct rt_resource_sample out;
    int ret = rt_mon_get_latest(&mon, &out);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "get_latest on empty returns NOENT");

    s1.cpu_util_pct  = 40;
    s1.latency_us_p99 = 200;
    s1.mem_used_kb   = 1024;
    ret = rt_mon_record(&mon, &s1);
    TEST_ASSERT(ret == HFSSS_OK, "record returns OK");

    ret = rt_mon_get_latest(&mon, &out);
    TEST_ASSERT(ret == HFSSS_OK, "get_latest returns OK");
    TEST_ASSERT(out.cpu_util_pct == 40, "cpu_util_pct matches");
    TEST_ASSERT(out.latency_us_p99 == 200, "latency matches");

    rt_mon_cleanup(&mon);
}

static void test_mon_anomaly_detection(void) {
    separator();
    printf("Test: anomaly detection thresholds\n");
    separator();

    struct rt_resource_monitor mon;
    rt_mon_init(&mon, 80, 1000);

    /* Normal sample: no anomalies */
    struct rt_resource_sample s = {.cpu_util_pct = 50, .latency_us_p99 = 500};
    rt_mon_record(&mon, &s);
    TEST_ASSERT(rt_mon_cpu_anomalies(&mon) == 0, "no CPU anomaly for 50%");
    TEST_ASSERT(rt_mon_latency_anomalies(&mon) == 0, "no latency anomaly for 500us");

    /* CPU anomaly */
    s.cpu_util_pct = 95;
    rt_mon_record(&mon, &s);
    TEST_ASSERT(rt_mon_cpu_anomalies(&mon) == 1, "CPU anomaly fired at 95%");

    /* Latency anomaly */
    s.cpu_util_pct   = 50;
    s.latency_us_p99 = 2000;
    rt_mon_record(&mon, &s);
    TEST_ASSERT(rt_mon_latency_anomalies(&mon) == 1, "latency anomaly fired at 2000us");

    rt_mon_cleanup(&mon);
}

static void test_mon_history_wrap(void) {
    separator();
    printf("Test: resource monitor history wraps correctly\n");
    separator();

    struct rt_resource_monitor mon;
    rt_mon_init(&mon, 0, 0);

    /* Record more than RT_MON_HISTORY_LEN samples */
    for (uint32_t i = 0; i < RT_MON_HISTORY_LEN + 5; i++) {
        struct rt_resource_sample s = {.cpu_util_pct = (uint32_t)(i % 100)};
        rt_mon_record(&mon, &s);
    }
    TEST_ASSERT(mon.sample_count == RT_MON_HISTORY_LEN, "sample_count capped at HISTORY_LEN");

    struct rt_resource_sample latest;
    int ret = rt_mon_get_latest(&mon, &latest);
    TEST_ASSERT(ret == HFSSS_OK, "get_latest returns OK after wrap");

    rt_mon_cleanup(&mon);
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void) {
    separator();
    printf("HFSSS RT Services Tests\n");
    separator();

    test_affinity_mask();
    test_pin_self();
    test_ipc_init_cleanup();
    test_ipc_send_recv();
    test_ipc_full();
    test_ipc_timeout();
    test_ipc_null_safety();
    test_trace_init_cleanup();
    test_trace_write_read();
    test_trace_empty_read();
    test_trace_multiple_entries();
    test_trace_null_safety();
    test_mon_init_cleanup();
    test_mon_record_get();
    test_mon_anomaly_detection();
    test_mon_history_wrap();

    separator();
    printf("Test Summary\n");
    separator();
    printf("  Total:  %d\n", total);
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);
    separator();

    if (failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    }
    printf("\n  [FAILURE] Some tests failed!\n");
    return 1;
}
