#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/proc_interface.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else       { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

/* Helper: check that a file exists and is non-empty */
static int file_nonempty(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return st.st_size > 0;
}

/* Helper: remove a file (ignore errors) */
static void rm_f(const char *path)
{
    unlink(path);
}

/* ------------------------------------------------------------------ */
static void test_proc_init_cleanup(void)
{
    printf("\n=== test_proc_init_cleanup ===\n");

    struct proc_interface proc;
    const char *dir = "/tmp/hfsss_test_proc1";
    struct stat st;
    int ret;

    /* NULL proc pointer must not crash */
    ret = proc_interface_init(NULL, dir);
    TEST_ASSERT(ret != 0, "init with NULL proc should fail");

    /* cleanup with NULL must not crash */
    proc_interface_cleanup(NULL);
    TEST_ASSERT(1, "cleanup with NULL proc does not crash");

    /* Normal init */
    ret = proc_interface_init(&proc, dir);
    TEST_ASSERT(ret == 0, "init with valid dir should succeed");
    TEST_ASSERT(proc.initialized == 1, "initialized flag set to 1 after init");

    /* Directory must exist */
    ret = stat(dir, &st);
    TEST_ASSERT(ret == 0 && S_ISDIR(st.st_mode), "proc dir created on disk");

    /* Cleanup zeroes struct */
    proc_interface_cleanup(&proc);
    TEST_ASSERT(proc.initialized == 0, "initialized flag cleared after cleanup");

    /* Cleanup idempotent (call twice) */
    proc_interface_cleanup(&proc);
    TEST_ASSERT(1, "double cleanup does not crash");

    rmdir(dir);
}

/* ------------------------------------------------------------------ */
static void test_proc_write_status(void)
{
    printf("\n=== test_proc_write_status ===\n");

    struct proc_interface proc;
    const char *dir = "/tmp/hfsss_test_proc2";
    char path[512];
    int ret;

    ret = proc_interface_init(&proc, dir);
    TEST_ASSERT(ret == 0, "init for status test");

    struct proc_status s;
    memset(&s, 0, sizeof(s));
    s.uptime_sec = 12345;
    snprintf(s.fw_version, sizeof(s.fw_version), "1.0.0-test");
    snprintf(s.state,      sizeof(s.state),      "RUNNING");

    ret = proc_write_status(&proc, &s);
    TEST_ASSERT(ret == 0, "proc_write_status returns 0");

    snprintf(path, sizeof(path), "%s/status", dir);
    TEST_ASSERT(file_nonempty(path), "status file exists and is non-empty");

    /* NULL safety */
    ret = proc_write_status(NULL, &s);
    TEST_ASSERT(ret != 0, "proc_write_status with NULL proc returns error");

    ret = proc_write_status(&proc, NULL);
    TEST_ASSERT(ret != 0, "proc_write_status with NULL status returns error");

    rm_f(path);
    proc_interface_cleanup(&proc);
    rmdir(dir);
}

/* ------------------------------------------------------------------ */
static void test_proc_write_perf(void)
{
    printf("\n=== test_proc_write_perf ===\n");

    struct proc_interface proc;
    const char *dir = "/tmp/hfsss_test_proc3";
    char path[512];
    int ret;

    ret = proc_interface_init(&proc, dir);
    TEST_ASSERT(ret == 0, "init for perf test");

    struct proc_perf_counters p;
    memset(&p, 0, sizeof(p));
    p.read_iops        = 500000;
    p.write_iops       = 200000;
    p.read_bw_mbps     = 7000;
    p.write_bw_mbps    = 3500;
    p.read_lat_p99_us  = 120;
    p.write_lat_p99_us = 250;

    ret = proc_write_perf_counters(&proc, &p);
    TEST_ASSERT(ret == 0, "proc_write_perf_counters returns 0");

    snprintf(path, sizeof(path), "%s/perf_counters", dir);
    TEST_ASSERT(file_nonempty(path), "perf_counters file exists and is non-empty");

    /* NULL safety */
    ret = proc_write_perf_counters(NULL, &p);
    TEST_ASSERT(ret != 0, "proc_write_perf_counters with NULL proc returns error");

    ret = proc_write_perf_counters(&proc, NULL);
    TEST_ASSERT(ret != 0, "proc_write_perf_counters with NULL perf returns error");

    rm_f(path);
    proc_interface_cleanup(&proc);
    rmdir(dir);
}

/* ------------------------------------------------------------------ */
static void test_proc_write_ftl_stats(void)
{
    printf("\n=== test_proc_write_ftl_stats ===\n");

    struct proc_interface proc;
    const char *dir = "/tmp/hfsss_test_proc4";
    char path[512];
    int ret;

    ret = proc_interface_init(&proc, dir);
    TEST_ASSERT(ret == 0, "init for ftl_stats test");

    struct proc_ftl_stats fs;
    memset(&fs, 0, sizeof(fs));
    fs.l2p_hit_rate_pct = 98;
    fs.gc_count         = 4200;
    fs.waf              = 1.35;
    fs.avg_erase_count  = 512;
    fs.max_erase_count  = 1024;

    ret = proc_write_ftl_stats(&proc, &fs);
    TEST_ASSERT(ret == 0, "proc_write_ftl_stats returns 0");

    snprintf(path, sizeof(path), "%s/ftl_stats", dir);
    TEST_ASSERT(file_nonempty(path), "ftl_stats file exists and is non-empty");

    /* NULL safety */
    ret = proc_write_ftl_stats(NULL, &fs);
    TEST_ASSERT(ret != 0, "proc_write_ftl_stats with NULL proc returns error");

    ret = proc_write_ftl_stats(&proc, NULL);
    TEST_ASSERT(ret != 0, "proc_write_ftl_stats with NULL stats returns error");

    rm_f(path);
    proc_interface_cleanup(&proc);
    rmdir(dir);
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("========================================\n");
    printf("Proc Interface Tests\n");
    printf("========================================\n");

    test_proc_init_cleanup();
    test_proc_write_status();
    test_proc_write_perf();
    test_proc_write_ftl_stats();

    printf("\n========================================\n");
    printf("Results: %d/%d passed", passed, total);
    if (failed > 0) {
        printf(", %d FAILED", failed);
    }
    printf("\n========================================\n");

    return (failed > 0) ? 1 : 0;
}
