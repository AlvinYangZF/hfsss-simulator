#ifndef __HFSSS_SPSC_RING_H
#define __HFSSS_SPSC_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include "common.h"

/*
 * REQ-085: Single-producer single-consumer lock-free ring buffer.
 *
 * Exactly one thread may call tryput; exactly one thread may call
 * tryget. head/tail use release/acquire so the produced element is
 * visible to the consumer before the consumer sees the updated
 * head index, and vice versa for tail.
 *
 * Capacity must be a power of two so slot indices can mask cheaply.
 * Queue occupancy is (head - tail); modular arithmetic on u32
 * tolerates wrap-around so the ring runs indefinitely.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct spsc_ring {
    u8              *buffer;
    u32              elem_size;
    u32              capacity;   /* power of two */
    u32              mask;       /* capacity - 1 */
    atomic_uint      head;       /* next producer write index */
    atomic_uint      tail;       /* next consumer read index */
};

int  spsc_ring_init    (struct spsc_ring *r, u32 elem_size, u32 capacity);
void spsc_ring_cleanup (struct spsc_ring *r);
int  spsc_ring_tryput  (struct spsc_ring *r, const void *elem);
int  spsc_ring_tryget  (struct spsc_ring *r, void *elem);
u32  spsc_ring_count   (const struct spsc_ring *r);
bool spsc_ring_empty   (const struct spsc_ring *r);
bool spsc_ring_full    (const struct spsc_ring *r);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_SPSC_RING_H */
