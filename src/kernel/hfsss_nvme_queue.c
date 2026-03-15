// SPDX-License-Identifier: GPL-2.0
/*
 * hfsss_nvme_queue.c — Admin and I/O queue management
 *                      (REQ-009, REQ-012, REQ-013, REQ-014)
 *
 * Queue topology
 * --------------
 *   Queue 0  : Admin queue  — carries Create/Delete I/O SQ, Create/Delete
 *              I/O CQ, Identify, Set/Get Features, and Abort commands.
 *   Queue 1–N: I/O queues   — one MSI-X interrupt vector per CQ; the number
 *              of queue pairs is bounded by min(HFSSS_MAX_IO_QUEUES,
 *              num_online_cpus()).
 *
 * MSI-X interrupt delivery
 * ------------------------
 *   pci_alloc_irq_vectors is called with HFSSS_MAX_IO_QUEUES + 1 vectors
 *   (one for admin, one per I/O queue).  Each CQ is assigned its own vector
 *   so that completion interrupts for different queues do not serialise.
 *
 * Interrupt coalescing (REQ-014)
 * ------------------------------
 *   Per-CQ coalescing is controlled by two parameters written via the
 *   Set Features (Feature ID 0x08 — Interrupt Coalescing) admin command:
 *     hfsss_coalesce_thr  — completion count threshold (0 = disabled)
 *     hfsss_coalesce_time — aggregation window in 100 µs units
 *   The hfsss_process_completion() call path checks these before raising the
 *   MSI-X interrupt.
 */

#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "hfsss_nvme_kmod.h"

/* Maximum number of I/O queue pairs (admin queue is always queue 0) */
#define HFSSS_MAX_IO_QUEUES   64

/* Default coalescing parameters (disabled) */
#define HFSSS_COALESCE_THR_DEFAULT   0
#define HFSSS_COALESCE_TIME_DEFAULT  0

/* Per-queue descriptor (admin and I/O share this structure) */
struct hfsss_queue {
    u16      qid;          /* queue identifier (0 = admin)              */
    u16      sq_size;      /* submission queue depth                    */
    u16      cq_size;      /* completion queue depth                    */
    int      irq_vector;   /* MSI-X vector assigned to this queue's CQ  */
    u32      coalesce_thr; /* interrupt coalescing completion threshold  */
    u32      coalesce_time;/* interrupt coalescing time window (100 µs)  */
    u32      pending_cpls; /* completions accumulated since last IRQ     */
    bool     allocated;
};

/* Static queue table — one admin + HFSSS_MAX_IO_QUEUES I/O queues */
static struct hfsss_queue g_queues[HFSSS_MAX_IO_QUEUES + 1];

/* MSI-X interrupt handler — wakes the blk-mq completion path */
static irqreturn_t hfsss_msix_handler(int irq, void *data)
{
    /*
     * In a full implementation:
     *   1. Identify which CQ fired (from the MSI-X vector index in *data).
     *   2. Call nvme_process_cq() / blk_mq_complete_request() for each
     *      completed entry in that CQ.
     *   3. Update the CQ head doorbell.
     *   4. Return IRQ_HANDLED.
     *
     * Stub: nothing to do until the blk-mq layer is wired up.
     */
    (void)irq;
    (void)data;
    return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------------
 * hfsss_admin_queue_create — allocate queue-0 (admin) resources
 *
 * The admin queue does not use MSI-X; it uses the pin-based INTx interrupt
 * or the first MSI-X vector (vector 0).  Queue depth is fixed at 32 entries
 * per NVMe spec requirement (MQES for admin is a separate field in AQA).
 * --------------------------------------------------------------------------*/
int hfsss_admin_queue_create(struct pci_dev *dev)
{
    struct hfsss_queue *q = &g_queues[0];

    if (q->allocated)
        return -EBUSY;

    /*
     * Allocate IRQ vectors for all queues in one shot.  The Linux NVMe driver
     * does the same: it requests max_queues vectors and uses however many the
     * PCI subsystem grants.
     */
    int nvecs = pci_alloc_irq_vectors(dev, 1, HFSSS_MAX_IO_QUEUES + 1,
                                      PCI_IRQ_MSIX | PCI_IRQ_MSI);
    if (nvecs < 0) {
        dev_warn(&dev->dev,
                 "hfsss_nvme: irq_vectors alloc failed (%d), falling back to INTx\n",
                 nvecs);
        /*
         * INTx fallback: still usable but all queues share one interrupt.
         * For production use MSI-X is strongly recommended (lower latency,
         * no shared-interrupt serialisation).
         */
        nvecs = 0;
    }

    q->qid           = 0;
    q->sq_size       = 32;
    q->cq_size       = 32;
    q->irq_vector    = (nvecs > 0) ? pci_irq_vector(dev, 0) : -1;
    q->coalesce_thr  = HFSSS_COALESCE_THR_DEFAULT;
    q->coalesce_time = HFSSS_COALESCE_TIME_DEFAULT;
    q->pending_cpls  = 0;
    q->allocated     = true;

    if (q->irq_vector >= 0) {
        int ret = request_irq(q->irq_vector, hfsss_msix_handler,
                              0, "hfsss_nvme-admin", q);
        if (ret) {
            dev_err(&dev->dev,
                    "hfsss_nvme: request_irq(admin) failed (%d)\n", ret);
            q->allocated = false;
            pci_free_irq_vectors(dev);
            return ret;
        }
    }

    dev_dbg(&dev->dev, "hfsss_nvme: admin queue created (vector=%d)\n",
            q->irq_vector);
    return 0;
}

/* ---------------------------------------------------------------------------
 * hfsss_admin_queue_destroy — release admin queue resources
 * --------------------------------------------------------------------------*/
void hfsss_admin_queue_destroy(struct pci_dev *dev)
{
    struct hfsss_queue *q = &g_queues[0];

    if (!q->allocated)
        return;

    if (q->irq_vector >= 0)
        free_irq(q->irq_vector, q);

    pci_free_irq_vectors(dev);
    memset(q, 0, sizeof(*q));
}

/* ---------------------------------------------------------------------------
 * hfsss_io_queue_create — allocate an I/O SQ/CQ pair
 *
 * Called in response to Create I/O Submission Queue / Create I/O Completion
 * Queue admin commands.  Each CQ gets its own MSI-X vector to enable
 * per-queue interrupt affinity pinning (set via /proc/irq/<n>/smp_affinity).
 *
 * @qid      : queue identifier in range [1, HFSSS_MAX_IO_QUEUES]
 * @sq_size  : number of 64-byte SQ entries
 * @cq_size  : number of 16-byte CQ entries
 * --------------------------------------------------------------------------*/
int hfsss_io_queue_create(struct pci_dev *dev, u16 qid,
                           u16 sq_size, u16 cq_size)
{
    struct hfsss_queue *q;

    if (qid == 0 || qid > HFSSS_MAX_IO_QUEUES)
        return -EINVAL;

    q = &g_queues[qid];
    if (q->allocated)
        return -EBUSY;

    /*
     * Request the MSI-X vector for this queue's CQ.  Vector 0 is reserved for
     * the admin queue; I/O queues start at vector 1.
     */
    int vec = pci_irq_vector(dev, qid);
    if (vec < 0) {
        dev_warn(&dev->dev,
                 "hfsss_nvme: no MSI-X vector for queue %u, sharing vector 0\n",
                 qid);
        vec = pci_irq_vector(dev, 0);
    }

    q->qid           = qid;
    q->sq_size       = sq_size;
    q->cq_size       = cq_size;
    q->irq_vector    = vec;
    q->coalesce_thr  = HFSSS_COALESCE_THR_DEFAULT;
    q->coalesce_time = HFSSS_COALESCE_TIME_DEFAULT;
    q->pending_cpls  = 0;
    q->allocated     = true;

    if (vec >= 0) {
        int ret = request_irq(vec, hfsss_msix_handler,
                              IRQF_SHARED, "hfsss_nvme-io", q);
        if (ret) {
            dev_err(&dev->dev,
                    "hfsss_nvme: request_irq(io qid=%u) failed (%d)\n",
                    qid, ret);
            q->allocated = false;
            return ret;
        }
    }

    dev_dbg(&dev->dev,
            "hfsss_nvme: I/O queue %u created (sq=%u cq=%u vector=%d)\n",
            qid, sq_size, cq_size, vec);
    return 0;
}

/* ---------------------------------------------------------------------------
 * hfsss_io_queue_destroy — release an I/O queue pair
 * --------------------------------------------------------------------------*/
void hfsss_io_queue_destroy(struct pci_dev *dev, u16 qid)
{
    struct hfsss_queue *q;

    if (qid == 0 || qid > HFSSS_MAX_IO_QUEUES)
        return;

    q = &g_queues[qid];
    if (!q->allocated)
        return;

    if (q->irq_vector >= 0)
        free_irq(q->irq_vector, q);

    memset(q, 0, sizeof(*q));
}

/* ---------------------------------------------------------------------------
 * hfsss_process_completion — drain CQ entries and raise MSI-X if warranted
 *
 * Intended to be called from the shared-memory poll thread
 * (hfsss_shmem_poll_completion) each time a batch of CQEs arrive from the
 * user-space simulator.
 *
 * Interrupt coalescing logic:
 *   If coalesce_thr == 0 the interrupt fires immediately for every completion.
 *   Otherwise the interrupt is deferred until pending_cpls >= coalesce_thr OR
 *   the coalesce_time window expires (timer not shown in this stub).
 * --------------------------------------------------------------------------*/
void hfsss_process_completion(struct pci_dev *dev, u16 qid, u32 n_cpls)
{
    struct hfsss_queue *q;

    if (qid > HFSSS_MAX_IO_QUEUES)
        return;

    q = &g_queues[qid];
    if (!q->allocated)
        return;

    q->pending_cpls += n_cpls;

    /*
     * Raise interrupt when coalescing threshold is reached (or immediately
     * if threshold is zero / coalescing is disabled).
     */
    if (q->coalesce_thr == 0 || q->pending_cpls >= q->coalesce_thr) {
        if (q->irq_vector >= 0) {
            /*
             * In a production implementation, pci_irq_vector + mmio write to
             * the MSI-X table entry would trigger the interrupt.  Here we call
             * the handler directly since there is no real hardware path.
             */
            hfsss_msix_handler(q->irq_vector, q);
        }
        q->pending_cpls = 0;
    }
}

#endif /* __KERNEL__ */
