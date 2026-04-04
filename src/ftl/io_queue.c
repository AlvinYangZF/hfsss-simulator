#include "ftl/io_queue.h"
#include <stdlib.h>
#include <string.h>

static inline u32 next_power_of_2(u32 v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

int io_ring_init(struct io_ring *ring, u32 slot_size, u32 capacity)
{
    if (!ring || slot_size == 0 || capacity == 0) {
        return HFSSS_ERR_INVAL;
    }

    capacity = next_power_of_2(capacity);

    ring->slots = calloc(capacity, slot_size);
    if (!ring->slots) {
        return HFSSS_ERR_NOMEM;
    }

    ring->slot_size = slot_size;
    ring->capacity = capacity;
    ring->head = 0;
    ring->tail = 0;
    return HFSSS_OK;
}

void io_ring_cleanup(struct io_ring *ring)
{
    if (!ring) return;
    free(ring->slots);
    memset(ring, 0, sizeof(*ring));
}

bool io_ring_push(struct io_ring *ring, const void *item)
{
    u32 tail = ring->tail;
    u32 next = (tail + 1) & (ring->capacity - 1);

    /* Full? */
    if (next == __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE)) {
        return false;
    }

    memcpy((u8 *)ring->slots + tail * ring->slot_size,
           item, ring->slot_size);

    __atomic_store_n(&ring->tail, next, __ATOMIC_RELEASE);
    return true;
}

bool io_ring_pop(struct io_ring *ring, void *item)
{
    u32 head = ring->head;

    /* Empty? */
    if (head == __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE)) {
        return false;
    }

    memcpy(item, (u8 *)ring->slots + head * ring->slot_size,
           ring->slot_size);

    __atomic_store_n(&ring->head, (head + 1) & (ring->capacity - 1),
                     __ATOMIC_RELEASE);
    return true;
}

bool io_ring_is_empty(struct io_ring *ring)
{
    return __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
}

bool io_ring_is_full(struct io_ring *ring)
{
    u32 next = (__atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) + 1)
               & (ring->capacity - 1);
    return next == __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
}

u32 io_ring_count(struct io_ring *ring)
{
    u32 head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    u32 tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    return (tail - head) & (ring->capacity - 1);
}
