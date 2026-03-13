#ifndef __HFSSS_SHMEM_IF_H
#define __HFSSS_SHMEM_IF_H

#include "common/common.h"

#define RING_BUFFER_SLOTS 16384
#define CMD_SLOT_SIZE 128
#define DATA_BUFFER_SIZE (128 * 1024 * 1024)

/* Command Type */
enum cmd_type {
    CMD_NVME_ADMIN = 0,
    CMD_NVME_IO = 1,
    CMD_CONTROL = 2,
};

/* NVMe Command (from kernel) */
struct nvme_cmd_from_kern {
    u32 cmd_type;
    u32 cmd_id;
    u32 sqid;
    u32 cqid;
    u64 prp1;
    u64 prp2;
    u32 cdw0_15[16];
    u32 data_len;
    u32 flags;
    u64 metadata;
};

/* Completion (to kernel) */
struct nvme_cpl_to_kern {
    u32 cmd_id;
    u16 sqid;
    u16 cqid;
    u16 sqhd;
    u16 cid;
    u32 status;
    u32 cdw0;
};

/* Ring Buffer Slot */
struct ring_slot {
    struct nvme_cmd_from_kern cmd;
    volatile u32 ready;
    volatile u32 done;
};

/* Ring Buffer Header */
struct ring_header {
    volatile u32 prod_idx;
    volatile u32 cons_idx;
    u32 slot_count;
    u32 slot_size;
    u64 prod_seq;
    u64 cons_seq;
};

/* Shared Memory Layout */
struct shmem_layout {
    struct ring_header header;
    struct ring_slot slots[RING_BUFFER_SLOTS];
    u8 data_buffer[DATA_BUFFER_SIZE];
};

/* Function Prototypes */
int shmem_if_open(const char *path, struct shmem_layout **shmem_out, int *fd_out);
void shmem_if_close(struct shmem_layout *shmem, int fd);
int shmem_if_receive_cmd(struct shmem_layout *shmem, struct nvme_cmd_from_kern *cmd);
int shmem_if_send_cpl(struct shmem_layout *shmem, struct nvme_cpl_to_kern *cpl);

#endif /* __HFSSS_SHMEM_IF_H */
