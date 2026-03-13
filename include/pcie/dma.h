#ifndef __HFSSS_PCIE_DMA_H
#define __HFSSS_PCIE_DMA_H

#include "common/common.h"
#include "common/mutex.h"
#include "pcie/queue.h"
#include "pcie/nvme.h"

/* DMA Transfer Direction */
enum dma_dir {
    DMA_DIR_HOST_TO_DEV = 0,  /* Read from host memory (write to device) */
    DMA_DIR_DEV_TO_HOST = 1,  /* Write to host memory (read from device) */
    DMA_DIR_BIDIRECTIONAL = 2,
};

/* DMA Transfer State */
enum dma_state {
    DMA_STATE_IDLE = 0,
    DMA_STATE_PENDING,
    DMA_STATE_IN_PROGRESS,
    DMA_STATE_COMPLETED,
    DMA_STATE_ERROR,
};

/* DMA Descriptor */
struct dma_desc {
    u64 src_addr;
    u64 dst_addr;
    u32 length;
    enum dma_dir dir;
    enum dma_state state;
    int error;
};

/* DMA Context */
struct dma_ctx {
    /* For user-space emulation: local buffer */
    u8 *buffer;
    u64 buffer_size;

    /* Statistics */
    u64 transfer_count;
    u64 bytes_transferred;
    u64 error_count;

    /* Lock for DMA operations */
    struct mutex lock;

    /* Parent device data */
    void *private_data;
};

/* Function Prototypes */
int dma_init(struct dma_ctx *ctx, u64 buffer_size);
void dma_cleanup(struct dma_ctx *ctx);

/* PRP/SGL to local buffer copy (user-space emulation) */
int dma_copy_from_prp(struct dma_ctx *ctx, u8 *dst, u64 prp1, u64 prp2, u32 len, u32 page_size);
int dma_copy_to_prp(struct dma_ctx *ctx, u64 prp1, u64 prp2, const u8 *src, u32 len, u32 page_size);
int dma_copy_from_sgl(struct dma_ctx *ctx, u8 *dst, const u8 *sgl, u32 sgl_len, u32 data_len);
int dma_copy_to_sgl(struct dma_ctx *ctx, u8 *sgl, u32 sgl_len, const u8 *src, u32 data_len);

/* Command data transfer helpers */
int dma_read_cmd_data(struct dma_ctx *ctx, struct nvme_sq_entry *cmd, u8 *buffer, u32 len, u32 page_size);
int dma_write_cmd_data(struct dma_ctx *ctx, struct nvme_sq_entry *cmd, const u8 *buffer, u32 len, u32 page_size);

#endif /* __HFSSS_PCIE_DMA_H */
