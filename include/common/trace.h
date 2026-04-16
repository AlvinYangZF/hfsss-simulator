/* Per-thread lockless trace ring for IO path data-flow debugging.
 * Enabled only when compiled with -DHFSSS_DEBUG_TRACE=1; otherwise a no-op.
 */
#ifndef HFSSS_COMMON_TRACE_H
#define HFSSS_COMMON_TRACE_H

#include <stdint.h>
#include <stddef.h>

enum trace_point_id {
    TRACE_POINT_T1_NBD_RECV = 1,
    TRACE_POINT_T2_WORKER_DEQ = 2,
    TRACE_POINT_T3_PPN_DONE = 3,
    TRACE_POINT_T4_PRE_HAL = 4,
    TRACE_POINT_T5_POST_HAL = 5,
};

enum trace_op {
    TRACE_OP_READ = 0,
    TRACE_OP_WRITE = 1,
    TRACE_OP_TRIM = 2,
};

#pragma pack(push, 1)
struct trace_record {
    uint64_t tsc;
    uint64_t lba;
    uint64_t ppn_or_len;
    uint32_t point_id;
    uint32_t op;
    uint32_t crc32c;
    uint32_t extra;
    uint32_t thread_id;
    uint32_t _pad;
}; /* 48 bytes */
#pragma pack(pop)

#ifdef HFSSS_DEBUG_TRACE

void trace_init(const char *dump_path);
void trace_shutdown(void);

void trace_emit(uint32_t point_id, uint32_t op, uint64_t lba,
                uint64_t ppn_or_len, uint32_t crc32c, uint32_t extra);

uint32_t trace_crc32c(const void *data, size_t len);

#define TRACE_EMIT(pid, op, lba, extra_u64, crc, extra_u32) \
    trace_emit((pid), (op), (lba), (extra_u64), (crc), (extra_u32))

#else /* !HFSSS_DEBUG_TRACE */

#define trace_init(p) ((void)0)
#define trace_shutdown() ((void)0)
#define TRACE_EMIT(pid, op, lba, extra_u64, crc, extra_u32) ((void)0)

static inline uint32_t trace_crc32c(const void *data, size_t len) {
    (void)data; (void)len; return 0;
}

#endif /* HFSSS_DEBUG_TRACE */

#endif /* HFSSS_COMMON_TRACE_H */
