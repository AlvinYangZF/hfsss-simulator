// SPDX-License-Identifier: GPL-2.0
/*
 * hfsss_nvme_shmem.c — Kernel-user space shared-memory ring buffer (REQ-022)
 *
 * Memory layout
 * -------------
 * The shared region is a contiguous set of pages allocated with alloc_pages.
 * It is mapped into the user-space simulator process via a /dev/hfsss_shmem
 * character device that implements .mmap (not shown in this stub).
 *
 *   Offset 0x000 : struct hfsss_shmem_hdr   (64 bytes, cache-line aligned)
 *   Offset 0x040 : submit ring              (HFSSS_SHMEM_RING_SIZE × cmd_size)
 *   Offset 0x040 + submit_ring_bytes
 *               : complete ring             (HFSSS_SHMEM_RING_SIZE × cpl_size)
 *
 * Ring buffer protocol
 * --------------------
 *   Submit ring (kernel → user-space):
 *     Kernel increments submit_head after writing a command slot.
 *     A smp_wmb() after the slot write ensures the command body is visible
 *     before the head index update.
 *     User-space polls submit_head != submit_tail; after reading a slot it
 *     increments submit_tail.
 *
 *   Complete ring (user-space → kernel):
 *     User-space increments complete_head after writing a completion slot.
 *     Kernel polls complete_head != complete_tail in hfsss_shmem_poll_completion.
 *     A smp_rmb() before reading the completion slot prevents speculative
 *     load of the slot before the head index is confirmed visible.
 *
 * Memory ordering summary
 * -----------------------
 *   Producer (submit):   write slot → smp_wmb() → write head index
 *   Consumer (submit):   read head index → smp_rmb() → read slot
 *   Producer (complete): write slot → smp_wmb() → write head index
 *   Consumer (complete): read head index → smp_rmb() → read slot
 */

#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/nvme.h>

#include "hfsss_nvme_kmod.h"

/* Kernel-side view of the shared memory header */
struct hfsss_shmem_hdr_k {
    u32 magic;
    u32 version;
    u32 ring_size;
    u32 _pad;
    u64 submit_head;    /* kernel writes (producer index)      */
    u64 submit_tail;    /* user-space reads (consumer index)   */
    u64 complete_head;  /* user-space writes (producer index)  */
    u64 complete_tail;  /* kernel reads (consumer index)       */
};

/* Module-level shmem state */
static struct page *g_shmem_pages;
static void        *g_shmem_virt;
static size_t       g_shmem_size;
static struct hfsss_shmem_hdr_k *g_hdr;
static struct nvme_command       *g_submit_ring;
static struct nvme_completion    *g_complete_ring;

/* ---------------------------------------------------------------------------
 * hfsss_shmem_init — allocate shared pages and initialise ring header
 *
 * @size: minimum byte size of the shared region; rounded up to PAGE_SIZE.
 *
 * alloc_pages(GFP_KERNEL, order) allocates 2^order physically contiguous
 * pages.  We compute the order from the requested size.  Physically
 * contiguous memory simplifies the user-space mmap path: a single
 * vm_insert_page loop suffices without an sg-list.
 * --------------------------------------------------------------------------*/
int hfsss_shmem_init(size_t size)
{
    unsigned int order;
    size_t total;

    /* Account for header plus both rings */
    total = sizeof(struct hfsss_shmem_hdr_k)
          + HFSSS_SHMEM_RING_SIZE * sizeof(struct nvme_command)
          + HFSSS_SHMEM_RING_SIZE * sizeof(struct nvme_completion);

    if (size > total)
        total = size;

    total = PAGE_ALIGN(total);
    order = get_order(total);

    g_shmem_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
    if (!g_shmem_pages) {
        pr_err("hfsss_nvme: alloc_pages(order=%u) failed for shmem\n", order);
        return -ENOMEM;
    }

    g_shmem_virt = page_address(g_shmem_pages);
    g_shmem_size = PAGE_SIZE << order;

    /* Carve out header and ring pointers from the flat buffer */
    g_hdr          = (struct hfsss_shmem_hdr_k *)g_shmem_virt;
    g_submit_ring  = (struct nvme_command *)
                     ((u8 *)g_shmem_virt + sizeof(struct hfsss_shmem_hdr_k));
    g_complete_ring = (struct nvme_completion *)
                      ((u8 *)g_submit_ring +
                       HFSSS_SHMEM_RING_SIZE * sizeof(struct nvme_command));

    /* Initialise header with magic/version so user-space can validate */
    g_hdr->magic       = HFSSS_SHMEM_MAGIC;
    g_hdr->version     = HFSSS_SHMEM_VERSION;
    g_hdr->ring_size   = HFSSS_SHMEM_RING_SIZE;
    g_hdr->_pad        = 0;
    g_hdr->submit_head = 0;
    g_hdr->submit_tail = 0;
    g_hdr->complete_head = 0;
    g_hdr->complete_tail = 0;

    pr_info("hfsss_nvme: shmem initialised (%zu KB, order=%u)\n",
            g_shmem_size / 1024, order);
    return 0;
}

/* ---------------------------------------------------------------------------
 * hfsss_shmem_cleanup — release shared pages
 * --------------------------------------------------------------------------*/
void hfsss_shmem_cleanup(void)
{
    if (!g_shmem_pages)
        return;

    unsigned int order = get_order(g_shmem_size);
    __free_pages(g_shmem_pages, order);

    g_shmem_pages   = NULL;
    g_shmem_virt    = NULL;
    g_shmem_size    = 0;
    g_hdr           = NULL;
    g_submit_ring   = NULL;
    g_complete_ring = NULL;

    pr_info("hfsss_nvme: shmem released\n");
}

/* ---------------------------------------------------------------------------
 * hfsss_shmem_submit — place an NVMe command in the submit ring
 *
 * Called from the blk-mq ->queue_rq path after translating a Linux bio into
 * an NVMe SQE.  Returns -ENOSPC if the ring is full (back-pressure to blk-mq).
 *
 * Memory ordering:
 *   Write the command slot first, then issue smp_wmb() to prevent the CPU or
 *   compiler from reordering the head-index update before the slot write.
 *   Without this barrier the user-space consumer could observe an incremented
 *   head but stale command data in the slot.
 * --------------------------------------------------------------------------*/
int hfsss_shmem_submit(const struct nvme_command *cmd)
{
    u64 head, tail, next;

    if (!g_hdr)
        return -ENODEV;

    head = READ_ONCE(g_hdr->submit_head);
    tail = READ_ONCE(g_hdr->submit_tail);
    next = (head + 1) % HFSSS_SHMEM_RING_SIZE;

    /* Ring is full when next head == tail (one slot reserved as sentinel) */
    if (next == tail) {
        pr_debug_ratelimited("hfsss_nvme: submit ring full\n");
        return -ENOSPC;
    }

    /* Copy command into the ring slot */
    memcpy(&g_submit_ring[head % HFSSS_SHMEM_RING_SIZE], cmd, sizeof(*cmd));

    /*
     * Store barrier: ensure command data is fully written before the head
     * index is updated and becomes visible to the user-space consumer.
     */
    smp_wmb();

    WRITE_ONCE(g_hdr->submit_head, next);
    return 0;
}

/* ---------------------------------------------------------------------------
 * hfsss_shmem_poll_completion — drain completed entries from the complete ring
 *
 * Called from a kernel thread (or timer callback) to pick up completions
 * posted by the user-space simulator.  For each CQE found, the function
 * invokes the blk-mq completion path and updates the complete_tail index.
 *
 * Returns the number of completions processed (0 if ring is empty).
 *
 * Memory ordering:
 *   Read the head index first, then issue smp_rmb() before reading the slot.
 *   This prevents the CPU from speculatively loading stale slot data before
 *   the producer's head update is confirmed as globally visible.
 * --------------------------------------------------------------------------*/
int hfsss_shmem_poll_completion(void)
{
    u64 head, tail;
    int processed = 0;

    if (!g_hdr)
        return -ENODEV;

    head = READ_ONCE(g_hdr->complete_head);
    tail = READ_ONCE(g_hdr->complete_tail);

    while (tail != head) {
        struct nvme_completion *cqe;

        /*
         * Load barrier: confirm that head is stable before reading the slot.
         * Prevents speculative loads of slot data before the producer's
         * smp_wmb() on the user-space side has propagated.
         */
        smp_rmb();

        cqe = &g_complete_ring[tail % HFSSS_SHMEM_RING_SIZE];

        /*
         * In a full implementation:
         *   nvme_complete_rq(find_rq_by_command_id(cqe->command_id), cqe);
         *
         * Stub: log the completion for tracing during bringup.
         */
        pr_debug("hfsss_nvme: cqe cmd_id=%u status=0x%04x\n",
                 cqe->command_id, le16_to_cpu(cqe->status));

        tail = (tail + 1) % HFSSS_SHMEM_RING_SIZE;
        processed++;

        /* Refresh head in case user-space added more completions */
        head = READ_ONCE(g_hdr->complete_head);
    }

    if (processed > 0)
        WRITE_ONCE(g_hdr->complete_tail, tail);

    return processed;
}

#endif /* __KERNEL__ */
