#ifndef __HFSSS_IO_QUEUE_H
#define __HFSSS_IO_QUEUE_H

#include "common/common.h"

/* I/O request opcode */
enum io_opcode {
    IO_OP_READ  = 0,
    IO_OP_WRITE = 1,
    IO_OP_TRIM  = 2,
    IO_OP_FLUSH = 3,
    IO_OP_STOP  = 4,   /* Sentinel: tells worker to exit */
};

/* I/O request — submitted from dispatch to FTL worker */
struct io_request {
    enum io_opcode opcode;
    u64            lba;
    u32            count;       /* Number of pages */
    u8            *data;        /* Pointer to caller buffer */
    u32            lba_stride;  /* LBA delta between pages in this request */
    u32            data_stride; /* Buffer delta between pages in this request */
    u32            data_offset; /* Offset within the parent request buffer */
    u32            byte_len;    /* Bytes covered by this sub-request */
    u64            nbd_handle;  /* Echoed back in completion */
};

/* I/O completion — returned from FTL worker to dispatch */
struct io_completion {
    u64  nbd_handle;
    u32  data_offset;
    u32  byte_len;
    int  status;
};

/*
 * SPSC ring buffer (Single Producer Single Consumer) — lock-free.
 * Used for dispatch → worker queues (1 producer, 1 consumer).
 */
#define IO_RING_DEFAULT_CAPACITY 4096

struct io_ring {
    void             *slots;
    u32               slot_size;
    u32               capacity;      /* Must be power of 2 */
    volatile u32      head;          /* Written by consumer */
    volatile u32      tail;          /* Written by producer */
};

int  io_ring_init(struct io_ring *ring, u32 slot_size, u32 capacity);
void io_ring_cleanup(struct io_ring *ring);
bool io_ring_push(struct io_ring *ring, const void *item);
bool io_ring_pop(struct io_ring *ring, void *item);
bool io_ring_is_empty(struct io_ring *ring);
bool io_ring_is_full(struct io_ring *ring);
u32  io_ring_count(struct io_ring *ring);

#endif /* __HFSSS_IO_QUEUE_H */
