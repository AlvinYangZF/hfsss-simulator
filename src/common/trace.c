/* Per-thread lockless trace ring. Each thread lazily allocates a ring on
 * first emit. A global registry (mutex-protected, contended only at ring
 * birth/death) tracks all rings; trace_shutdown walks the registry and
 * dumps each ring to the configured file path in a single pass.
 */
#ifdef HFSSS_DEBUG_TRACE

#include "common/trace.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

#define TRACE_RING_CAPACITY (64 * 1024) /* 64K records/thread * 48B = 3 MiB */

struct trace_ring {
    struct trace_record recs[TRACE_RING_CAPACITY];
    atomic_ulong head;   /* next write slot; monotonically increasing */
    uint32_t thread_id;
    struct trace_ring *next;
};

static __thread struct trace_ring *tls_ring = NULL;
static struct trace_ring *g_ring_list = NULL;
static pthread_mutex_t g_ring_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_dump_path[512];
static atomic_uint g_next_tid = 0;

static uint64_t read_tsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void free_tls_ring(void *ring)
{
    (void)ring;
    /* rings are freed by trace_shutdown; TLS destructor not needed */
}

static struct trace_ring *get_or_create_ring(void)
{
    if (tls_ring) return tls_ring;
    struct trace_ring *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->thread_id = atomic_fetch_add(&g_next_tid, 1) + 1;
    atomic_store(&r->head, 0);
    pthread_mutex_lock(&g_ring_mtx);
    r->next = g_ring_list;
    g_ring_list = r;
    pthread_mutex_unlock(&g_ring_mtx);
    tls_ring = r;
    return r;
}

void trace_init(const char *dump_path)
{
    if (dump_path) {
        strncpy(g_dump_path, dump_path, sizeof(g_dump_path) - 1);
        g_dump_path[sizeof(g_dump_path) - 1] = '\0';
    }
}

void trace_emit(uint32_t point_id, uint32_t op, uint64_t lba,
                uint64_t ppn_or_len, uint32_t crc32c, uint32_t extra)
{
    struct trace_ring *r = get_or_create_ring();
    if (!r) return;
    unsigned long slot = atomic_fetch_add(&r->head, 1) % TRACE_RING_CAPACITY;
    struct trace_record *rec = &r->recs[slot];
    rec->tsc = read_tsc();
    rec->lba = lba;
    rec->ppn_or_len = ppn_or_len;
    rec->point_id = point_id;
    rec->op = op;
    rec->crc32c = crc32c;
    rec->extra = extra;
    rec->thread_id = r->thread_id;
    rec->_pad = 0;
}

void trace_shutdown(void)
{
    if (g_dump_path[0] == '\0') return;
    FILE *f = fopen(g_dump_path, "wb");
    if (!f) return;
    pthread_mutex_lock(&g_ring_mtx);
    for (struct trace_ring *r = g_ring_list; r; r = r->next) {
        unsigned long head = atomic_load(&r->head);
        unsigned long count = head < TRACE_RING_CAPACITY ? head : TRACE_RING_CAPACITY;
        unsigned long start = head < TRACE_RING_CAPACITY ? 0 : head % TRACE_RING_CAPACITY;
        for (unsigned long i = 0; i < count; i++) {
            unsigned long idx = (start + i) % TRACE_RING_CAPACITY;
            fwrite(&r->recs[idx], sizeof(struct trace_record), 1, f);
        }
    }
    pthread_mutex_unlock(&g_ring_mtx);
    fclose(f);
    /* Free rings. */
    pthread_mutex_lock(&g_ring_mtx);
    struct trace_ring *r = g_ring_list;
    while (r) {
        struct trace_ring *n = r->next;
        free(r);
        r = n;
    }
    g_ring_list = NULL;
    tls_ring = NULL;
    pthread_mutex_unlock(&g_ring_mtx);
    (void)free_tls_ring; /* silence unused */
}

/* Software CRC32C (Castagnoli). Correct and stable; speed is not critical
 * because trace is a debug-only build.
 */
uint32_t trace_crc32c(const void *data, size_t len)
{
    static const uint32_t POLY = 0x82F63B78u;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (POLY & mask);
        }
    }
    return ~crc;
}

#endif /* HFSSS_DEBUG_TRACE */
