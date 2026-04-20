/*
 * REQ-085: SPSC lock-free ring. See header for ordering guarantees.
 *
 * Producer reads head relaxed (it owns head) and tail acquire to
 * synchronize with the consumer's release on the previous get; the
 * write lands before head is published with release. Consumer does
 * the mirror image. No locks, no system calls on the hot path.
 */

#include "common/spsc_ring.h"
#include <stdlib.h>
#include <string.h>

/* True iff n is a power of two and non-zero. */
static inline bool is_pow2(u32 n)
{
    return n != 0 && (n & (n - 1)) == 0;
}

int spsc_ring_init(struct spsc_ring *r, u32 elem_size, u32 capacity)
{
    if (!r || elem_size == 0 || !is_pow2(capacity)) {
        return HFSSS_ERR_INVAL;
    }
    /* Guard against capacity * elem_size overflowing size_t on the
     * allocation path. A ring sized beyond what size_t can address
     * can't be represented even for a conforming calloc, and the
     * same overflow would corrupt slot-offset arithmetic on every
     * tryput/tryget thereafter. */
    if ((size_t)capacity > (size_t)-1 / (size_t)elem_size) {
        return HFSSS_ERR_INVAL;
    }
    memset(r, 0, sizeof(*r));
    r->buffer = (u8 *)calloc(capacity, elem_size);
    if (!r->buffer) {
        return HFSSS_ERR_NOMEM;
    }
    r->elem_size = elem_size;
    r->capacity  = capacity;
    r->mask      = capacity - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return HFSSS_OK;
}

void spsc_ring_cleanup(struct spsc_ring *r)
{
    if (!r) return;
    free(r->buffer);
    memset(r, 0, sizeof(*r));
}

int spsc_ring_tryput(struct spsc_ring *r, const void *elem)
{
    if (!r || !r->buffer || !elem) {
        return HFSSS_ERR_INVAL;
    }
    unsigned h = atomic_load_explicit(&r->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&r->tail, memory_order_acquire);
    /* `h - t` computed modulo 2^32; correct while capacity <= 2^31
     * (we only support u32 capacities anyway). */
    if ((unsigned)(h - t) >= r->capacity) {
        return HFSSS_ERR_NOSPC;
    }
    /* Promote the index to size_t before multiplying by elem_size.
     * The naive `(u32) slot * elem_size` expression is evaluated in
     * u32 arithmetic — for large rings (capacity * elem_size past
     * 2^32) the product wraps and the producer and consumer write
     * to the wrong byte offsets. */
    size_t off = (size_t)(h & r->mask) * (size_t)r->elem_size;
    memcpy(r->buffer + off, elem, r->elem_size);
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return HFSSS_OK;
}

int spsc_ring_tryget(struct spsc_ring *r, void *elem)
{
    if (!r || !r->buffer || !elem) {
        return HFSSS_ERR_INVAL;
    }
    unsigned t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (h == t) {
        return HFSSS_ERR_AGAIN;
    }
    /* Same size_t promotion as tryput — see comment there. */
    size_t off = (size_t)(t & r->mask) * (size_t)r->elem_size;
    memcpy(elem, r->buffer + off, r->elem_size);
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return HFSSS_OK;
}

u32 spsc_ring_count(const struct spsc_ring *r)
{
    if (!r) return 0;
    unsigned h = atomic_load_explicit(&r->head, memory_order_acquire);
    unsigned t = atomic_load_explicit(&r->tail, memory_order_acquire);
    return (u32)(h - t);
}

bool spsc_ring_empty(const struct spsc_ring *r)
{
    return spsc_ring_count(r) == 0;
}

bool spsc_ring_full(const struct spsc_ring *r)
{
    if (!r) return false;
    return spsc_ring_count(r) >= r->capacity;
}
