#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include "common/trace.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

#ifdef HFSSS_DEBUG_TRACE

/* Read all trace_record entries from a dump file. Returns allocated buffer
 * and record count in *out_n. Caller frees the buffer. */
static struct trace_record *read_dump(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        *out_n = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (size_t)size % sizeof(struct trace_record) != 0) {
        fclose(f);
        *out_n = 0;
        return NULL;
    }
    size_t n = (size_t)size / sizeof(struct trace_record);
    struct trace_record *buf =
        (struct trace_record *)malloc(n * sizeof(struct trace_record));
    if (!buf) {
        fclose(f);
        *out_n = 0;
        return NULL;
    }
    size_t got = fread(buf, sizeof(struct trace_record), n, f);
    fclose(f);
    if (got != n) {
        free(buf);
        *out_n = 0;
        return NULL;
    }
    *out_n = n;
    return buf;
}

/* ----------------------------------------------------------------------- */
/* crc32c-stable test                                                      */
/* ----------------------------------------------------------------------- */
static void test_crc32c_stable(void)
{
    printf("\n=== trace: crc32c stable ===\n");
    const char *s = "hello world";
    uint32_t a = trace_crc32c(s, 11);
    uint32_t b = trace_crc32c(s, 11);
    TEST_ASSERT(a == b, "same input -> same crc");
    TEST_ASSERT(a != 0, "crc is non-zero for non-empty input");

    /* Known-value smoke: 0 bytes = 0 (empty message) */
    TEST_ASSERT(trace_crc32c("", 0) == 0, "crc32c(empty) == 0");

    /* Different input -> different crc (very likely). */
    uint32_t c = trace_crc32c("hello WORLD", 11);
    TEST_ASSERT(a != c, "different input -> different crc");
}

/* ----------------------------------------------------------------------- */
/* single-thread content test: dump records, verify fields round-trip      */
/* ----------------------------------------------------------------------- */
static void test_record_content_roundtrip(void)
{
    printf("\n=== trace: record content round-trip ===\n");
    const char *path = "/tmp/test_trace_content.bin";
    unlink(path);
    trace_init(path);
    /* Emit 3 records with distinguishable fields. */
    TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, TRACE_OP_WRITE,
               0xAAAA1111ULL, 4096ULL, 0xDEADBEEFu, 7u);
    TRACE_EMIT(TRACE_POINT_T3_PPN_DONE, TRACE_OP_READ,
               0xBBBB2222ULL, 0x123456789ABCDEFULL, 0u, 42u);
    TRACE_EMIT(TRACE_POINT_T5_POST_HAL, TRACE_OP_WRITE,
               0ULL, 0xCAFEBABEULL, 0x12345678u, 1u);
    trace_shutdown();

    size_t n = 0;
    struct trace_record *recs = read_dump(path, &n);
    TEST_ASSERT(recs != NULL, "content dump opened");
    TEST_ASSERT(n == 3, "content dump has 3 records");
    if (recs && n == 3) {
        /* Records share one thread -> ordered by emit order. */
        TEST_ASSERT(recs[0].point_id == TRACE_POINT_T1_NBD_RECV,
                    "record[0] point_id = T1");
        TEST_ASSERT(recs[0].op == TRACE_OP_WRITE,
                    "record[0] op = WRITE");
        TEST_ASSERT(recs[0].lba == 0xAAAA1111ULL,
                    "record[0] lba preserved");
        TEST_ASSERT(recs[0].ppn_or_len == 4096ULL,
                    "record[0] ppn_or_len preserved");
        TEST_ASSERT(recs[0].crc32c == 0xDEADBEEFu,
                    "record[0] crc32c preserved");
        TEST_ASSERT(recs[0].extra == 7u,
                    "record[0] extra preserved");
        TEST_ASSERT(recs[1].point_id == TRACE_POINT_T3_PPN_DONE,
                    "record[1] point_id = T3");
        TEST_ASSERT(recs[1].ppn_or_len == 0x123456789ABCDEFULL,
                    "record[1] ppn_or_len preserved (64-bit)");
        TEST_ASSERT(recs[2].point_id == TRACE_POINT_T5_POST_HAL,
                    "record[2] point_id = T5");
        /* All three in the same thread should share thread_id (>= 1). */
        TEST_ASSERT(recs[0].thread_id == recs[1].thread_id &&
                    recs[1].thread_id == recs[2].thread_id,
                    "single-thread records share thread_id");
        TEST_ASSERT(recs[0].thread_id >= 1u,
                    "thread_id assigned (>=1)");
        /* Monotonic tsc within a thread. */
        TEST_ASSERT(recs[0].tsc <= recs[1].tsc &&
                    recs[1].tsc <= recs[2].tsc,
                    "monotonic tsc within thread");
    }
    free(recs);
}

/* ----------------------------------------------------------------------- */
/* multi-thread dump test                                                  */
/* ----------------------------------------------------------------------- */
static void *mt_worker(void *arg)
{
    int tid = *(int *)arg;
    for (int i = 0; i < 100; i++) {
        TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, TRACE_OP_WRITE,
                   /* lba */ (uint64_t)tid * 1000 + (uint64_t)i,
                   /* ppn_or_len */ 4096ULL,
                   /* crc */ 0xDEADBEEFu ^ (uint32_t)tid,
                   /* extra */ (uint32_t)tid);
    }
    return NULL;
}

static void test_multi_thread_dump(void)
{
    printf("\n=== trace: multi-thread dump ===\n");
    const char *path = "/tmp/test_trace_mt.bin";
    unlink(path);
    trace_init(path);
    pthread_t t[4];
    int ids[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, mt_worker, &ids[i]);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    trace_shutdown();

    size_t n = 0;
    struct trace_record *recs = read_dump(path, &n);
    TEST_ASSERT(recs != NULL, "mt dump opened");
    TEST_ASSERT(n == 400, "mt dump has 400 records");
    if (recs && n == 400) {
        /* Count records per thread_id — distinct values per producing thread.
         * Each of 4 producers emits 100, so we expect exactly 4 distinct
         * thread_ids with count 100 each. */
        uint32_t tids_seen[8] = {0};
        uint32_t tids_count[8] = {0};
        int tids_n = 0;
        for (size_t i = 0; i < n; i++) {
            int found = -1;
            for (int k = 0; k < tids_n; k++) {
                if (tids_seen[k] == recs[i].thread_id) { found = k; break; }
            }
            if (found < 0) {
                if (tids_n < 8) {
                    tids_seen[tids_n] = recs[i].thread_id;
                    tids_count[tids_n] = 1;
                    tids_n++;
                }
            } else {
                tids_count[found]++;
            }
        }
        TEST_ASSERT(tids_n == 4, "mt dump has exactly 4 distinct thread_ids");
        int all_100 = 1;
        for (int k = 0; k < tids_n; k++) {
            if (tids_count[k] != 100) { all_100 = 0; break; }
        }
        TEST_ASSERT(all_100, "every producing thread emitted 100 records");
    }
    free(recs);
}

/* ----------------------------------------------------------------------- */
/* wrap-around test: emit > RING_CAPACITY records in a single thread, see  */
/* that the ring keeps only the last CAPACITY emissions. The ring capacity */
/* is internal (64K) so we emit a large N and verify:                      */
/*   - dumped count equals min(emitted, 64K)                               */
/*   - the retained records are the most recent ones (by crc32c field)     */
/* ----------------------------------------------------------------------- */
static void *wrap_worker(void *arg)
{
    uint32_t total_emits = *(uint32_t *)arg;
    for (uint32_t i = 0; i < total_emits; i++) {
        TRACE_EMIT(TRACE_POINT_T4_PRE_HAL, TRACE_OP_WRITE,
                   (uint64_t)i, 0ULL, i, 0u);
    }
    return NULL;
}

static void test_ring_wraparound(void)
{
    printf("\n=== trace: ring wraparound ===\n");
    const char *path = "/tmp/test_trace_wrap.bin";
    unlink(path);
    trace_init(path);

    /* Emit more than one ring capacity (64K) so wraparound happens. */
    uint32_t total_emits = 100000u;
    pthread_t t;
    pthread_create(&t, NULL, wrap_worker, &total_emits);
    pthread_join(t, NULL);
    trace_shutdown();

    size_t n = 0;
    struct trace_record *recs = read_dump(path, &n);
    TEST_ASSERT(recs != NULL, "wrap dump opened");
    /* With 64K ring and 100K emissions, we expect exactly 64K records
     * retained (the most recent). */
    TEST_ASSERT(n == 65536, "wrap dump retains exactly 64K records");
    if (recs && n > 0) {
        /* The oldest retained record should be emission index
         * (total_emits - 64K) = 100000 - 65536 = 34464, because
         * crc32c field was used to stash the emission index. */
        uint32_t oldest_idx = total_emits - (uint32_t)n;
        uint32_t newest_idx = total_emits - 1u;
        TEST_ASSERT(recs[0].crc32c == oldest_idx,
                    "first retained record is oldest-in-ring");
        TEST_ASSERT(recs[n - 1].crc32c == newest_idx,
                    "last retained record is newest");
    }
    free(recs);
}

/* ----------------------------------------------------------------------- */
/* teardown safety: calling trace_init / shutdown twice must not crash     */
/* and must reset state between runs.                                      */
/* ----------------------------------------------------------------------- */
static void test_teardown_safety(void)
{
    printf("\n=== trace: teardown safety ===\n");
    const char *p1 = "/tmp/test_trace_td1.bin";
    const char *p2 = "/tmp/test_trace_td2.bin";
    unlink(p1);
    unlink(p2);

    trace_init(p1);
    TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, TRACE_OP_READ, 1ULL, 2ULL, 3u, 4u);
    trace_shutdown();

    trace_init(p2);
    TRACE_EMIT(TRACE_POINT_T2_WORKER_DEQ, TRACE_OP_WRITE, 10ULL, 20ULL, 30u, 40u);
    trace_shutdown();

    size_t n1 = 0, n2 = 0;
    struct trace_record *r1 = read_dump(p1, &n1);
    struct trace_record *r2 = read_dump(p2, &n2);
    TEST_ASSERT(r1 != NULL && n1 == 1, "first run dumped 1 record");
    TEST_ASSERT(r2 != NULL && n2 == 1, "second run dumped 1 record");
    if (r1 && n1 == 1) {
        TEST_ASSERT(r1[0].point_id == TRACE_POINT_T1_NBD_RECV,
                    "first dump has only first-run point");
    }
    if (r2 && n2 == 1) {
        TEST_ASSERT(r2[0].point_id == TRACE_POINT_T2_WORKER_DEQ,
                    "second dump has only second-run point");
    }
    free(r1);
    free(r2);
}

int main(void)
{
    test_crc32c_stable();
    test_record_content_roundtrip();
    test_multi_thread_dump();
    test_ring_wraparound();
    test_teardown_safety();
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
