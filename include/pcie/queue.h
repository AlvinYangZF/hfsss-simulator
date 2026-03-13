#ifndef __HFSSS_PCIE_QUEUE_H
#define __HFSSS_PCIE_QUEUE_H

#include "common/common.h"
#include "common/mutex.h"
#include "pcie/nvme.h"

/* Maximum queue pairs */
#define MAX_QUEUE_PAIRS 64

/* Queue Sizes */
#define ADMIN_QUEUE_SIZE 256
#define IO_QUEUE_SIZE    1024

/* PRP Constants */
#define PRP_PAGE_SIZE 4096
#define PRP_ENTRY_SIZE 8

/* SGL Descriptor Types */
#define SGL_DESC_TYPE_DATA   0x00
#define SGL_DESC_TYPE_BIT    0x01
#define SGL_DESC_TYPE_SEG    0x02
#define SGL_DESC_TYPE_LAST   0x03

/* SGL Subtypes */
#define SGL_SUBTYPE_ADDRESS  0x00
#define SGL_SUBTYPE_OFFSET   0x01
#define SGL_SUBTYPE_HOST_ID  0x02

/* PRP Walker State */
struct prp_walker {
    u64 prp1;
    u64 prp2;
    u32 offset;
    u32 length;
    u32 page_size;
    u32 current_page;
    u32 bytes_left;
};

/* SGL Segment */
struct sgl_segment {
    u8 type;
    u8 subtype;
    u8 flags;
    u8 rsvd;
    u32 length;
    u64 address;
};

/* SGL Walker State */
struct sgl_walker {
    void *sgl_base;
    u32 sgl_len;
    u32 current_seg;
    u32 seg_offset;
    u32 bytes_left;
};

/* NVMe Submission Queue */
struct nvme_sq {
    u16 qid;
    u16 qsize;
    u32 sq_head;
    u32 sq_tail;
    u64 base_addr;
    u32 entry_size;
    u16 cqid;
    u8 priority;
    bool enabled;

    /* For user-space emulation: local buffer */
    void *entries;

    /* Statistics */
    u64 cmd_count;
    u64 doorbell_writes;
};

/* NVMe Completion Queue */
struct nvme_cq {
    u16 qid;
    u16 qsize;
    u32 cq_head;
    u32 cq_tail;
    u64 base_addr;
    u32 entry_size;
    u8 phase;
    bool enabled;
    bool interrupt_enabled;

    /* For user-space emulation: local buffer */
    void *entries;

    /* Associated SQs */
    u16 associated_sqs[MAX_QUEUE_PAIRS];
    u32 num_associated_sqs;

    /* Statistics */
    u64 cpl_count;
    u64 interrupt_count;
};

/* Queue Pair */
struct nvme_queue_pair {
    u16 qid;
    struct nvme_sq *sq;
    struct nvme_cq *cq;
    bool active;
};

/* Queue Manager Context */
struct nvme_queue_mgr {
    /* Admin Queue Pair */
    struct nvme_sq admin_sq;
    struct nvme_cq admin_cq;

    /* I/O Queue Pairs */
    struct nvme_sq io_sqs[MAX_QUEUE_PAIRS];
    struct nvme_cq io_cqs[MAX_QUEUE_PAIRS];
    u32 num_io_sqs;
    u32 num_io_cqs;

    /* Lock for queue operations */
    struct mutex lock;

    /* Parent controller */
    void *ctrl;
};

/* Function Prototypes */
int nvme_queue_mgr_init(struct nvme_queue_mgr *mgr, void *ctrl);
void nvme_queue_mgr_cleanup(struct nvme_queue_mgr *mgr);

/* Submission Queue Operations */
int nvme_sq_create(struct nvme_sq *sq, u16 qid, u64 base_addr, u16 qsize, u32 entry_size, u16 cqid);
void nvme_sq_destroy(struct nvme_sq *sq);
int nvme_sq_update_tail(struct nvme_sq *sq, u32 new_tail);
int nvme_sq_fetch_cmd(struct nvme_sq *sq, struct nvme_sq_entry *cmd);
bool nvme_sq_has_pending(struct nvme_sq *sq);

/* Completion Queue Operations */
int nvme_cq_create(struct nvme_cq *cq, u16 qid, u64 base_addr, u16 qsize, u32 entry_size, bool intr_en);
void nvme_cq_destroy(struct nvme_cq *cq);
int nvme_cq_update_head(struct nvme_cq *cq, u32 new_head);
int nvme_cq_post_cpl(struct nvme_cq *cq, struct nvme_cq_entry *cpl);
bool nvme_cq_needs_interrupt(struct nvme_cq *cq);

/* PRP/SGL Operations */
int prp_walker_init(struct prp_walker *walker, u64 prp1, u64 prp2, u32 length, u32 page_size);
int prp_walker_next(struct prp_walker *walker, u64 *addr, u32 *len);
int prp_walker_skip(struct prp_walker *walker, u32 len);

int sgl_walker_init(struct sgl_walker *walker, void *sgl_base, u32 sgl_len);
int sgl_walker_next(struct sgl_walker *walker, u64 *addr, u32 *len);
int sgl_walker_skip(struct sgl_walker *walker, u32 len);

/* Queue Pair Operations */
int nvme_create_io_sq(struct nvme_queue_mgr *mgr, u16 qid, u64 base_addr, u16 qsize, u16 cqid, u8 prio);
int nvme_delete_io_sq(struct nvme_queue_mgr *mgr, u16 qid);
int nvme_create_io_cq(struct nvme_queue_mgr *mgr, u16 qid, u64 base_addr, u16 qsize, bool intr_en);
int nvme_delete_io_cq(struct nvme_queue_mgr *mgr, u16 qid);

#endif /* __HFSSS_PCIE_QUEUE_H */
