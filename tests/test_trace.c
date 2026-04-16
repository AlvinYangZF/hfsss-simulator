#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "common/trace.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

#ifdef HFSSS_DEBUG_TRACE

static void *worker(void *arg)
{
    int tid = *(int *)arg;
    for (int i = 0; i < 100; i++) {
        TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, TRACE_OP_WRITE,
                   /* lba */ (uint64_t)tid * 1000 + i,
                   /* ppn_or_len */ 4096,
                   /* crc */ 0xdeadbeef,
                   /* extra */ (uint32_t)tid);
    }
    return NULL;
}

static void test_multi_thread_dump(void)
{
    printf("\n=== trace: multi-thread dump ===\n");
    const char *path = "/tmp/test_trace_dump.bin";
    unlink(path);
    trace_init(path);
    pthread_t t[4];
    int ids[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, &ids[i]);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    trace_shutdown();
    FILE *f = fopen(path, "rb");
    TEST_ASSERT(f != NULL, "dump file created");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        TEST_ASSERT(size == 4 * 100 * (long)sizeof(struct trace_record),
                    "dump file has 400 records");
    }
}

static void test_crc32c_stable(void)
{
    printf("\n=== trace: crc32c stable ===\n");
    const char *s = "hello world";
    uint32_t a = trace_crc32c(s, 11);
    uint32_t b = trace_crc32c(s, 11);
    TEST_ASSERT(a == b, "same input -> same crc");
    TEST_ASSERT(a != 0, "crc is non-zero for non-empty input");
}

int main(void)
{
    test_crc32c_stable();
    test_multi_thread_dump();
    printf("\nResults: %d/%d passed, %d failed\n", passed, total, failed);
    return failed > 0 ? 1 : 0;
}

#else

int main(void)
{
    printf("trace instrumentation disabled (HFSSS_DEBUG_TRACE=0), skipping\n");
    return 0;
}

#endif
