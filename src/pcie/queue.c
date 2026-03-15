#include "pcie/queue.h"
#include <stdlib.h>
#include <string.h>

int nvme_queue_mgr_init(struct nvme_queue_mgr *mgr, void *ctrl)
{
    int ret;

    if (!mgr) {
        return HFSSS_ERR_INVAL;
    }

    memset(mgr, 0, sizeof(*mgr));

    ret = mutex_init(&mgr->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    mgr->ctrl = ctrl;
    mgr->num_io_sqs = 0;
    mgr->num_io_cqs = 0;

    return HFSSS_OK;
}

void nvme_queue_mgr_cleanup(struct nvme_queue_mgr *mgr)
{
    if (!mgr) {
        return;
    }

    mutex_cleanup(&mgr->lock);
    memset(mgr, 0, sizeof(*mgr));
}

int nvme_sq_create(struct nvme_sq *sq, u16 qid, u64 base_addr, u16 qsize, u32 entry_size, u16 cqid)
{
    if (!sq) {
        return HFSSS_ERR_INVAL;
    }

    memset(sq, 0, sizeof(*sq));

    sq->qid = qid;
    sq->base_addr = base_addr;
    sq->qsize = qsize;
    sq->entry_size = entry_size;
    sq->cqid = cqid;
    sq->sq_head = 0;
    sq->sq_tail = 0;
    sq->enabled = true;

    /* Allocate local buffer for emulation */
    sq->entries = calloc(qsize, entry_size);
    if (!sq->entries) {
        return HFSSS_ERR_NOMEM;
    }

    return HFSSS_OK;
}

void nvme_sq_destroy(struct nvme_sq *sq)
{
    if (!sq) {
        return;
    }

    if (sq->entries) {
        free(sq->entries);
    }

    memset(sq, 0, sizeof(*sq));
}

int nvme_sq_update_tail(struct nvme_sq *sq, u32 new_tail)
{
    if (!sq || !sq->enabled) {
        return HFSSS_ERR_INVAL;
    }

    if (new_tail >= sq->qsize) {
        return HFSSS_ERR_INVAL;
    }

    sq->sq_tail = new_tail;
    sq->doorbell_writes++;
    return HFSSS_OK;
}

int nvme_sq_fetch_cmd(struct nvme_sq *sq, struct nvme_sq_entry *cmd)
{
    if (!sq || !sq->enabled || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    if (sq->sq_head == sq->sq_tail) {
        return HFSSS_ERR_AGAIN;
    }

    /* Copy command from local buffer */
    memcpy(cmd, (u8 *)sq->entries + sq->sq_head * sq->entry_size, sizeof(*cmd));

    sq->sq_head = (sq->sq_head + 1) % sq->qsize;
    sq->cmd_count++;

    return HFSSS_OK;
}

bool nvme_sq_has_pending(struct nvme_sq *sq)
{
    if (!sq || !sq->enabled) {
        return false;
    }
    return sq->sq_head != sq->sq_tail;
}

int nvme_cq_create(struct nvme_cq *cq, u16 qid, u64 base_addr, u16 qsize, u32 entry_size, bool intr_en)
{
    if (!cq) {
        return HFSSS_ERR_INVAL;
    }

    memset(cq, 0, sizeof(*cq));

    cq->qid = qid;
    cq->base_addr = base_addr;
    cq->qsize = qsize;
    cq->entry_size = entry_size;
    cq->cq_head = 0;
    cq->cq_tail = 0;
    cq->phase = 1;
    cq->enabled = true;
    cq->interrupt_enabled = intr_en;

    /* Allocate local buffer for emulation */
    cq->entries = calloc(qsize, entry_size);
    if (!cq->entries) {
        return HFSSS_ERR_NOMEM;
    }

    return HFSSS_OK;
}

void nvme_cq_destroy(struct nvme_cq *cq)
{
    if (!cq) {
        return;
    }

    if (cq->entries) {
        free(cq->entries);
    }

    memset(cq, 0, sizeof(*cq));
}

int nvme_cq_update_head(struct nvme_cq *cq, u32 new_head)
{
    if (!cq || !cq->enabled) {
        return HFSSS_ERR_INVAL;
    }

    if (new_head >= cq->qsize) {
        return HFSSS_ERR_INVAL;
    }

    cq->cq_head = new_head;
    return HFSSS_OK;
}

int nvme_cq_post_cpl(struct nvme_cq *cq, struct nvme_cq_entry *cpl)
{
    if (!cq || !cq->enabled || !cpl) {
        return HFSSS_ERR_INVAL;
    }

    /* Set phase bit */
    cpl->status &= ~NVME_STATUS_PHASE_BIT;
    cpl->status |= cq->phase;

    /* Copy completion to local buffer */
    memcpy((u8 *)cq->entries + cq->cq_tail * cq->entry_size, cpl, sizeof(*cpl));

    cq->cq_tail = (cq->cq_tail + 1) % cq->qsize;
    if (cq->cq_tail == cq->cq_head) {
        /* Toggle phase bit when wrapping */
        cq->phase ^= 1;
    }

    cq->cpl_count++;
    if (cq->interrupt_enabled) {
        cq->interrupt_count++;
    }

    return HFSSS_OK;
}

bool nvme_cq_needs_interrupt(struct nvme_cq *cq)
{
    if (!cq || !cq->enabled) {
        return false;
    }
    return cq->interrupt_enabled && cq->cq_head != cq->cq_tail;
}

int prp_walker_init(struct prp_walker *walker, u64 prp1, u64 prp2, u32 length, u32 page_size)
{
    if (!walker) {
        return HFSSS_ERR_INVAL;
    }

    memset(walker, 0, sizeof(*walker));

    walker->prp1 = prp1;
    walker->prp2 = prp2;
    walker->length = length;
    walker->page_size = page_size;
    walker->bytes_left = length;
    walker->offset = 0;
    walker->current_page = 0;

    return HFSSS_OK;
}

int prp_walker_next(struct prp_walker *walker, u64 *addr, u32 *len)
{
    if (!walker || !addr || !len) {
        return HFSSS_ERR_INVAL;
    }

    if (walker->bytes_left == 0) {
        return HFSSS_ERR_NOENT;
    }

    u32 page_offset = walker->prp1 % walker->page_size;
    u32 bytes_in_page = walker->page_size - page_offset;

    if (walker->current_page == 0) {
        /* First page: PRP1 */
        *addr = walker->prp1;
        *len = MIN(walker->bytes_left, bytes_in_page);
        walker->offset += *len;
        walker->bytes_left -= *len;
        walker->current_page++;
        return HFSSS_OK;
    } else {
        /* Subsequent pages: PRP2 or PRP list */
        *addr = walker->prp2 + (walker->current_page - 1) * walker->page_size;
        *len = MIN(walker->bytes_left, walker->page_size);
        walker->offset += *len;
        walker->bytes_left -= *len;
        walker->current_page++;
        return HFSSS_OK;
    }
}

int prp_walker_skip(struct prp_walker *walker, u32 len)
{
    if (!walker) {
        return HFSSS_ERR_INVAL;
    }

    if (len > walker->bytes_left) {
        return HFSSS_ERR_INVAL;
    }

    walker->bytes_left -= len;
    walker->offset += len;
    return HFSSS_OK;
}

int sgl_walker_init(struct sgl_walker *walker, void *sgl_base, u32 sgl_len)
{
    if (!walker) {
        return HFSSS_ERR_INVAL;
    }

    memset(walker, 0, sizeof(*walker));

    walker->sgl_base = sgl_base;
    walker->sgl_len = sgl_len;
    walker->current_seg = 0;
    walker->seg_offset = 0;
    walker->bytes_left = 0;

    return HFSSS_OK;
}

int sgl_walker_next(struct sgl_walker *walker, u64 *addr, u32 *len)
{
    /* Simple SGL implementation for user-space */
    if (!walker || !addr || !len) {
        return HFSSS_ERR_INVAL;
    }
    return HFSSS_ERR_NOTSUPP;
}

int sgl_walker_skip(struct sgl_walker *walker, u32 len)
{
    if (!walker) {
        return HFSSS_ERR_INVAL;
    }
    return HFSSS_ERR_NOTSUPP;
}

int nvme_create_io_sq(struct nvme_queue_mgr *mgr, u16 qid, u64 base_addr, u16 qsize, u16 cqid, u8 prio)
{
    if (!mgr || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    if (mgr->io_sqs[qid].enabled) {
        return HFSSS_ERR_EXIST;
    }

    if (!mgr->io_cqs[cqid].enabled) {
        return HFSSS_ERR_NOENT;
    }

    int ret = nvme_sq_create(&mgr->io_sqs[qid], qid, base_addr, qsize, 64, cqid);
    if (ret == HFSSS_OK) {
        mgr->io_sqs[qid].priority = prio;
        mgr->num_io_sqs++;
        mgr->io_cqs[cqid].associated_sqs[mgr->io_cqs[cqid].num_associated_sqs++] = qid;
    }
    return ret;
}

int nvme_delete_io_sq(struct nvme_queue_mgr *mgr, u16 qid)
{
    if (!mgr || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    if (!mgr->io_sqs[qid].enabled) {
        return HFSSS_ERR_NOENT;
    }

    u16 cqid = mgr->io_sqs[qid].cqid;
    nvme_sq_destroy(&mgr->io_sqs[qid]);
    mgr->num_io_sqs--;

    /* Remove from CQ's associated list */
    if (mgr->io_cqs[cqid].enabled) {
        for (u32 i = 0; i < mgr->io_cqs[cqid].num_associated_sqs; i++) {
            if (mgr->io_cqs[cqid].associated_sqs[i] == qid) {
                for (u32 j = i; j < mgr->io_cqs[cqid].num_associated_sqs - 1; j++) {
                    mgr->io_cqs[cqid].associated_sqs[j] = mgr->io_cqs[cqid].associated_sqs[j + 1];
                }
                mgr->io_cqs[cqid].num_associated_sqs--;
                break;
            }
        }
    }
    return HFSSS_OK;
}

int nvme_create_io_cq(struct nvme_queue_mgr *mgr, u16 qid, u64 base_addr, u16 qsize, bool intr_en)
{
    if (!mgr || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    if (mgr->io_cqs[qid].enabled) {
        return HFSSS_ERR_EXIST;
    }

    int ret = nvme_cq_create(&mgr->io_cqs[qid], qid, base_addr, qsize, 16, intr_en);
    if (ret == HFSSS_OK) {
        mgr->num_io_cqs++;
    }
    return ret;
}

int nvme_delete_io_cq(struct nvme_queue_mgr *mgr, u16 qid)
{
    if (!mgr || qid == 0 || qid >= MAX_QUEUE_PAIRS) {
        return HFSSS_ERR_INVAL;
    }

    if (!mgr->io_cqs[qid].enabled) {
        return HFSSS_ERR_NOENT;
    }

    /* Check for associated SQs */
    for (int i = 0; i < MAX_QUEUE_PAIRS; i++) {
        if (mgr->io_sqs[i].enabled && mgr->io_sqs[i].cqid == qid) {
            return HFSSS_ERR_BUSY;
        }
    }

    nvme_cq_destroy(&mgr->io_cqs[qid]);
    mgr->num_io_cqs--;
    return HFSSS_OK;
}
