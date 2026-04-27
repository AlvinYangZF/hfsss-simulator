#ifndef __HFSSS_IO_ERR_TRACE_H
#define __HFSSS_IO_ERR_TRACE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

/* Env-gated single-line trace at every error-return site on the I/O path.
 * Enable with HFSSS_TRACE_IO_ERR=1; default off (getenv miss is cached so
 * the on/off path costs one relaxed load per call). Production builds are
 * unaffected when the env var is absent. */
static inline int hfsss_io_err_trace_enabled(void)
{
    static _Atomic int s_state = -1;
    int e = atomic_load_explicit(&s_state, memory_order_relaxed);
    if (e == -1) {
        const char *v = getenv("HFSSS_TRACE_IO_ERR");
        e = (v && v[0] != '\0' && v[0] != '0') ? 1 : 0;
        atomic_store_explicit(&s_state, e, memory_order_relaxed);
    }
    return e;
}

#define IO_ERR_TRACE(fmt, ...) do { \
    if (hfsss_io_err_trace_enabled()) { \
        fprintf(stderr, "[IO_ERR] " fmt "\n", ##__VA_ARGS__); \
    } \
} while (0)

#endif /* __HFSSS_IO_ERR_TRACE_H */
