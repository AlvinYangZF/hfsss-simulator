#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common/oob.h"
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
 * JSON-RPC helpers
 * ------------------------------------------------------------------ */
static void test_jsonrpc_build(void) {
    separator();
    printf("Test: JSON-RPC response builder\n");
    separator();

    char buf[512];

    /* Success response */
    int n = oob_jsonrpc_ok(buf, sizeof(buf), 42, "{\"state\":\"running\"}");
    TEST_ASSERT(n > 0, "jsonrpc_ok returns positive length");
    TEST_ASSERT(strstr(buf, "\"jsonrpc\":\"2.0\"") != NULL, "contains jsonrpc version");
    TEST_ASSERT(strstr(buf, "\"id\":42") != NULL, "contains id");
    TEST_ASSERT(strstr(buf, "\"result\"") != NULL, "contains result");
    TEST_ASSERT(strstr(buf, "\"state\":\"running\"") != NULL, "result payload preserved");

    /* Error response */
    n = oob_jsonrpc_err(buf, sizeof(buf), 1, -32601, "Method not found");
    TEST_ASSERT(n > 0, "jsonrpc_err returns positive length");
    TEST_ASSERT(strstr(buf, "\"error\"") != NULL, "contains error field");
    TEST_ASSERT(strstr(buf, "-32601") != NULL, "contains error code");
    TEST_ASSERT(strstr(buf, "Method not found") != NULL, "contains error message");

    /* null result */
    n = oob_jsonrpc_ok(buf, sizeof(buf), 0, NULL);
    TEST_ASSERT(strstr(buf, "\"result\":null") != NULL, "null result serialized correctly");
}

static void test_jsonrpc_parse(void) {
    separator();
    printf("Test: JSON-RPC request parser\n");
    separator();

    const char *req = "{\"jsonrpc\":\"2.0\",\"method\":\"status.get\",\"params\":{},\"id\":7}";
    char method[64];
    int64_t id;
    char params[64];

    int ret = oob_jsonrpc_parse(req, method, sizeof(method), &id, params, sizeof(params));
    TEST_ASSERT(ret == 0, "parse returns 0 for valid request");
    TEST_ASSERT(strcmp(method, "status.get") == 0, "method parsed correctly");
    TEST_ASSERT(id == 7, "id parsed correctly");

    /* Missing method */
    ret = oob_jsonrpc_parse("{\"jsonrpc\":\"2.0\",\"id\":1}",
                            method, sizeof(method), &id, params, sizeof(params));
    TEST_ASSERT(ret != 0, "parse fails when method is absent");

    /* NULL input */
    ret = oob_jsonrpc_parse(NULL, method, sizeof(method), &id, params, sizeof(params));
    TEST_ASSERT(ret != 0, "parse fails on NULL input");
}

/* ------------------------------------------------------------------
 * SMART log
 * ------------------------------------------------------------------ */
static void test_smart_init(void) {
    separator();
    printf("Test: SMART log init and defaults\n");
    separator();

    struct nvme_smart_log log;
    smart_init(&log);

    TEST_ASSERT(log.available_spare == 100, "initial spare = 100%");
    TEST_ASSERT(log.available_spare_thresh == SMART_DEFAULT_SPARE_THRESH,
                "spare threshold set to default");
    TEST_ASSERT(log.critical_warning == 0, "no warnings initially");
    int temp_c = (int)log.temperature - (int)SMART_KELVIN_OFFSET;
    TEST_ASSERT(temp_c == TEMP_AMBIENT_CELSIUS, "initial temperature is ambient");
}

static void test_smart_spare(void) {
    separator();
    printf("Test: SMART spare warning threshold\n");
    separator();

    struct nvme_smart_log log;
    smart_init(&log);

    smart_set_spare(&log, 50);
    TEST_ASSERT(log.available_spare == 50, "spare set to 50%");
    TEST_ASSERT(!(log.critical_warning & SMART_CRIT_SPARE),
                "no spare warning at 50%");

    smart_set_spare(&log, 5);
    TEST_ASSERT(log.critical_warning & SMART_CRIT_SPARE,
                "spare warning fires when spare < threshold");

    smart_set_spare(&log, 100);
    TEST_ASSERT(!(log.critical_warning & SMART_CRIT_SPARE),
                "spare warning clears when spare restored");
}

static void test_smart_temperature(void) {
    separator();
    printf("Test: SMART temperature and warning\n");
    separator();

    struct nvme_smart_log log;
    smart_init(&log);

    smart_set_temperature(&log, 50);
    TEST_ASSERT(log.temperature == (uint16_t)(50 + SMART_KELVIN_OFFSET),
                "temperature stored in Kelvin");
    TEST_ASSERT(!(log.critical_warning & SMART_CRIT_TEMPERATURE),
                "no temp warning at 50°C");

    smart_set_temperature(&log, 72);
    TEST_ASSERT(log.critical_warning & SMART_CRIT_TEMPERATURE,
                "temp warning fires at 72°C (>= 70°C threshold)");

    smart_set_temperature(&log, 40);
    TEST_ASSERT(!(log.critical_warning & SMART_CRIT_TEMPERATURE),
                "temp warning clears at 40°C");
}

static void test_smart_counters(void) {
    separator();
    printf("Test: SMART counter increments\n");
    separator();

    struct nvme_smart_log log;
    smart_init(&log);

    smart_increment_power_cycles(&log);
    smart_increment_power_cycles(&log);
    TEST_ASSERT(log.power_cycles[0] == 2, "power_cycles incremented");

    smart_increment_unsafe_shutdowns(&log);
    TEST_ASSERT(log.unsafe_shutdowns[0] == 1, "unsafe_shutdowns incremented");

    smart_add_media_error(&log);
    TEST_ASSERT(log.media_errors[0] == 1, "media_errors incremented");
    TEST_ASSERT(log.num_err_log_entries[0] == 1, "num_err_log_entries incremented");
    TEST_ASSERT(log.critical_warning & SMART_CRIT_DEGRADED,
                "DEGRADED bit set after media error");
}

static void test_smart_to_json(void) {
    separator();
    printf("Test: SMART JSON serialization\n");
    separator();

    struct nvme_smart_log log;
    smart_init(&log);
    smart_set_spare(&log, 75);
    smart_set_temperature(&log, 45);

    char *js = smart_to_json(&log);
    TEST_ASSERT(js != NULL, "smart_to_json returns non-NULL");
    TEST_ASSERT(strstr(js, "\"available_spare\":75") != NULL,
                "JSON contains available_spare");
    TEST_ASSERT(strstr(js, "\"temperature_celsius\":45") != NULL,
                "JSON contains temperature_celsius");
    TEST_ASSERT(strstr(js, "\"critical_warning\"") != NULL,
                "JSON contains critical_warning");
    TEST_ASSERT(strstr(js, "\"power_cycles\"") != NULL,
                "JSON contains power_cycles field");
    free(js);

    /* NULL safety */
    TEST_ASSERT(smart_to_json(NULL) == NULL, "smart_to_json(NULL) returns NULL");
}

/* ------------------------------------------------------------------
 * Performance counters
 * ------------------------------------------------------------------ */
static void test_perf_init(void) {
    separator();
    printf("Test: perf_counters init/cleanup\n");
    separator();

    struct perf_counters pc;
    int ret = perf_init(&pc);
    TEST_ASSERT(ret == HFSSS_OK, "perf_init returns OK");
    TEST_ASSERT(pc.initialized, "perf initialized");
    TEST_ASSERT(pc.total_read_ios == 0, "initial read IOs = 0");

    perf_cleanup(&pc);
    TEST_ASSERT(!pc.initialized, "perf not initialized after cleanup");

    TEST_ASSERT(perf_init(NULL) == HFSSS_ERR_INVAL, "perf_init(NULL) returns INVAL");
}

static void test_perf_record(void) {
    separator();
    printf("Test: perf record and snapshot\n");
    separator();

    struct perf_counters pc;
    perf_init(&pc);

    /* Record reads at 100µs latency */
    for (int i = 0; i < 100; i++)
        perf_record_io(&pc, true,  4096, 100000);  /* 100µs */
    /* Record writes at 200µs */
    for (int i = 0; i < 50; i++)
        perf_record_io(&pc, false, 4096, 200000);

    TEST_ASSERT(pc.total_read_ios  == 100, "total_read_ios = 100");
    TEST_ASSERT(pc.total_write_ios == 50,  "total_write_ios = 50");
    TEST_ASSERT(pc.total_read_bytes  == 100 * 4096, "total_read_bytes correct");
    TEST_ASSERT(pc.total_write_bytes == 50  * 4096, "total_write_bytes correct");

    struct perf_snapshot snap;
    perf_snapshot(&pc, &snap);
    TEST_ASSERT(snap.lat_read_p50_us  > 0,  "read p50 > 0");
    TEST_ASSERT(snap.lat_read_p99_us  >= snap.lat_read_p50_us,
                "p99 >= p50");
    TEST_ASSERT(snap.waf >= 1.0, "WAF >= 1.0");

    perf_cleanup(&pc);
}

static void test_perf_reset_window(void) {
    separator();
    printf("Test: perf window reset\n");
    separator();

    struct perf_counters pc;
    perf_init(&pc);

    for (int i = 0; i < 10; i++)
        perf_record_io(&pc, true, 4096, 50000);
    TEST_ASSERT(pc.window_read_ios == 10, "window_read_ios = 10");

    perf_reset_window(&pc);
    TEST_ASSERT(pc.window_read_ios == 0,  "window_read_ios = 0 after reset");
    TEST_ASSERT(pc.total_read_ios  == 10, "total_read_ios preserved");

    perf_cleanup(&pc);
}

/* ------------------------------------------------------------------
 * Temperature model
 * ------------------------------------------------------------------ */
static void test_temp_model(void) {
    separator();
    printf("Test: temperature model\n");
    separator();

    struct temp_model tm;
    struct nvme_smart_log log;
    smart_init(&log);
    temp_init(&tm, TEMP_AMBIENT_CELSIUS);

    TEST_ASSERT(tm.current_celsius == TEMP_AMBIENT_CELSIUS,
                "initial temp = ambient");
    TEST_ASSERT(!tm.throttle_active, "no throttle initially");

    /* Light load: no temp warning */
    bool changed = temp_update(&tm, &log, 10000.0, 500.0);
    TEST_ASSERT(tm.current_celsius > TEMP_AMBIENT_CELSIUS, "temp rises under load");
    (void)changed;

    /* Heavy load: above critical */
    temp_update(&tm, &log, 5000000.0, 100000.0);
    TEST_ASSERT(tm.current_celsius >= TEMP_CRIT_CELSIUS,
                "temp reaches critical under very heavy load");
    TEST_ASSERT(tm.throttle_active, "throttle active at critical temp");

    /* Cool down */
    bool cooled = temp_update(&tm, &log, 0.0, 0.0);
    TEST_ASSERT(tm.current_celsius == TEMP_AMBIENT_CELSIUS,
                "temp returns to ambient at idle");
    TEST_ASSERT(!tm.throttle_active, "throttle deactivated at ambient");
    TEST_ASSERT(cooled, "temp_update returns true when throttle state changes");
}

/* ------------------------------------------------------------------
 * OOB dispatch (no server, no socket)
 * ------------------------------------------------------------------ */
static void test_oob_dispatch(void) {
    separator();
    printf("Test: OOB dispatch without server\n");
    separator();

    struct oob_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.lock, NULL);
    smart_init(&ctx.stats.smart);
    perf_init(&ctx.stats.perf);
    temp_init(&ctx.stats.temp, TEMP_AMBIENT_CELSIUS);
    ctx.start_ns = 0;
    ctx.stats.nand_capacity_bytes = (uint64_t)4 * 1024 * 1024 * 1024ULL;
    ctx.stats.free_blocks_pct     = 85;

    char resp[2048];

    /* status.get */
    int ret = oob_dispatch(&ctx,
        "{\"jsonrpc\":\"2.0\",\"method\":\"status.get\",\"params\":{},\"id\":1}",
        resp, sizeof(resp));
    TEST_ASSERT(ret == HFSSS_OK, "dispatch status.get returns OK");
    TEST_ASSERT(strstr(resp, "\"result\"") != NULL, "response has result");
    TEST_ASSERT(strstr(resp, "\"state\":\"running\"") != NULL,
                "status.get returns state");

    /* smart.get */
    ret = oob_dispatch(&ctx,
        "{\"jsonrpc\":\"2.0\",\"method\":\"smart.get\",\"params\":{},\"id\":2}",
        resp, sizeof(resp));
    TEST_ASSERT(ret == HFSSS_OK, "dispatch smart.get returns OK");
    TEST_ASSERT(strstr(resp, "\"available_spare\"") != NULL,
                "smart.get response contains available_spare");

    /* perf.get */
    ret = oob_dispatch(&ctx,
        "{\"jsonrpc\":\"2.0\",\"method\":\"perf.get\",\"params\":{},\"id\":3}",
        resp, sizeof(resp));
    TEST_ASSERT(ret == HFSSS_OK, "dispatch perf.get returns OK");
    TEST_ASSERT(strstr(resp, "\"read_iops\"") != NULL,
                "perf.get response contains read_iops");

    /* Unknown method */
    ret = oob_dispatch(&ctx,
        "{\"jsonrpc\":\"2.0\",\"method\":\"nonexistent\",\"params\":{},\"id\":9}",
        resp, sizeof(resp));
    TEST_ASSERT(ret != HFSSS_OK, "unknown method returns error");
    TEST_ASSERT(strstr(resp, "-32601") != NULL, "error code -32601 in response");

    /* Parse error */
    ret = oob_dispatch(&ctx, "not-valid-json", resp, sizeof(resp));
    TEST_ASSERT(ret != HFSSS_OK, "parse error detected");
    TEST_ASSERT(strstr(resp, "-32700") != NULL, "error code -32700 in response");

    perf_cleanup(&ctx.stats.perf);
    pthread_mutex_destroy(&ctx.lock);
}

/* ------------------------------------------------------------------
 * OOB server round-trip (start server, connect, send, receive)
 * ------------------------------------------------------------------ */
static void test_oob_server_roundtrip(void) {
    separator();
    printf("Test: OOB server socket round-trip\n");
    separator();

    const char *sock_path = "/tmp/hfsss_test_oob.sock";
    struct oob_ctx ctx;

    int ret = oob_init(&ctx, sock_path);
    TEST_ASSERT(ret == HFSSS_OK, "oob_init returns OK");
    if (ret != HFSSS_OK) return;

    /* Connect as client */
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(cfd >= 0, "client socket created");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    ret = connect(cfd, (struct sockaddr *)&addr, sizeof(addr));
    TEST_ASSERT(ret == 0, "client connected to OOB server");

    if (ret == 0) {
        const char *req = "{\"jsonrpc\":\"2.0\",\"method\":\"status.get\","
                          "\"params\":{},\"id\":99}\n";
        send(cfd, req, strlen(req), 0);

        char resp[2048] = {0};
        /* Read response with timeout */
        struct timeval tv = {1, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(cfd, resp, sizeof(resp) - 1, 0);
        TEST_ASSERT(n > 0, "received response from server");
        if (n > 0) {
            resp[n] = '\0';
            TEST_ASSERT(strstr(resp, "\"jsonrpc\"") != NULL,
                        "response is valid JSON-RPC");
            TEST_ASSERT(strstr(resp, "\"result\"") != NULL,
                        "response contains result");
            TEST_ASSERT(strstr(resp, "99") != NULL, "response echoes id=99");
        }
        close(cfd);
    }

    oob_shutdown(&ctx);
    TEST_ASSERT(ctx.state == OOB_STATE_STOPPED, "server stopped after shutdown");
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void) {
    separator();
    printf("HFSSS OOB Management Tests\n");
    separator();

    test_jsonrpc_build();
    test_jsonrpc_parse();
    test_smart_init();
    test_smart_spare();
    test_smart_temperature();
    test_smart_counters();
    test_smart_to_json();
    test_perf_init();
    test_perf_record();
    test_perf_reset_window();
    test_temp_model();
    test_oob_dispatch();
    test_oob_server_roundtrip();

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
