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
