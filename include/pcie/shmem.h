#ifndef __HFSSS_PCIE_SHMEM_H
#define __HFSSS_PCIE_SHMEM_H

#include "common/common.h"
#include "common/mutex.h"
#include "pcie/nvme.h"

/* Shared Memory Constants */
#define SHMEM_PATH_DEFAULT "/dev/shm/hfsss_shmem"
#define SHMEM_SIZE (16 * 1024 * 1024)  /* 16MB */

/* Ring Buffer Constants */
#define RING_BUFFER_SLOTS 16384
#define RING_BUFFER_SLOT_SIZE 128

/* Command Types (Kernel <-> User-space) */
enum shmem_cmd_type {
    SHMEM_CMD_NVME_ADMIN = 0,
    SHMEM_CMD_NVME_IO,
    SHMEM_CMD_RESPONSE,
    SHMEM_CMD_NOTIFY,
    SHMEM_CMD_RESET,
};

/* Command Slot */
struct shmem_cmd_slot {
    u32 slot_id;
    u32 type;
    u32 flags;
    u32 status;

    u64 timestamp;
    u32 cmd_len;
    u32 resp_len;

    /* NVMe Command */
    struct nvme_sq_entry nvme_cmd;
    struct nvme_cq_entry nvme_cpl;

    /* Data buffer offset in shared memory */
    u64 data_offset;
    u32 data_len;

    u32 reserved[8];
} __attribute__((packed));

/* Ring Buffer Header */
struct ring_buffer_header {
    u32 magic;
    u32 version;
    u32 flags;
    u32 slot_count;
    u32 slot_size;

    volatile u32 producer_idx;
    volatile u32 consumer_idx;
    volatile u32 overflow_count;

    u64 total_produced;
    u64 total_consumed;
} __attribute__((packed));

#define RING_BUFFER_MAGIC 0x48465353  /* "HFSS" */
#define RING_BUFFER_VERSION 0x00010000

/* Shared Memory Layout */
struct shmem_layout {
    /* Ring Buffer Header */
    struct ring_buffer_header header;

    /* Command Slots */
    struct shmem_cmd_slot slots[RING_BUFFER_SLOTS];

    /* Data Buffer Area */
    u8 data_buffer[SHMEM_SIZE - sizeof(struct ring_buffer_header) -
                    sizeof(struct shmem_cmd_slot) * RING_BUFFER_SLOTS];
} __attribute__((packed));

/* Shared Memory Context */
struct shmem_ctx {
    /* Shared memory file descriptor */
    int fd;
    const char *path;

    /* Mapped memory */
    struct shmem_layout *layout;
    u64 size;

    /* Role (producer/consumer) */
    bool is_producer;
    bool is_consumer;

    /* Local state */
    u32 last_producer_idx;
    u32 last_consumer_idx;

    /* Eventfd for notifications */
    int eventfd_producer;
    int eventfd_consumer;

    /* Lock */
    struct mutex lock;

    /* Statistics */
    u64 sent_count;
    u64 recv_count;
    u64 drop_count;
};

/* Function Prototypes */
int shmem_create(struct shmem_ctx *ctx, const char *path);
int shmem_open(struct shmem_ctx *ctx, const char *path);
void shmem_close(struct shmem_ctx *ctx);

/* Producer Operations */
int shmem_produce_cmd(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd);
int shmem_produce_cmd_with_data(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd,
                                  const void *data, u32 data_len);

/* Consumer Operations */
int shmem_consume_cmd(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd);
int shmem_consume_cmd_with_data(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd,
                                  void *data, u32 *data_len);

/* Response Operations */
int shmem_send_response(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd);
int shmem_wait_for_response(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd, u64 timeout_ns);

/* Notification Operations */
int shmem_notify_consumer(struct shmem_ctx *ctx);
int shmem_notify_producer(struct shmem_ctx *ctx);

/* Helper: Convert NVMe command to shared memory slot */
void shmem_nvme_to_slot(struct shmem_cmd_slot *slot, const struct nvme_sq_entry *cmd,
                          u16 qid);
void shmem_slot_to_nvme(struct nvme_sq_entry *cmd, const struct shmem_cmd_slot *slot);

#endif /* __HFSSS_PCIE_SHMEM_H */
