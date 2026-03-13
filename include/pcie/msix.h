#ifndef __HFSSS_PCIE_MSIX_H
#define __HFSSS_PCIE_MSIX_H

#include "common/common.h"
#include "common/mutex.h"

/* MSI-X Constants */
#define MSIX_MAX_VECTORS 2048
#define MSIX_TABLE_ENTRY_SIZE 16
#define MSIX_PBA_ENTRY_SIZE 8

/* MSI-X Table Entry */
struct msix_table_entry {
    u32 msg_addr_low;
    u32 msg_addr_high;
    u32 msg_data;
    u32 vector_ctrl;
} __attribute__((packed));

#define MSIX_VECTOR_CTRL_MASK 0x00000001

/* MSI-X PBA (Pending Bit Array) */
struct msix_pba {
    u64 pending[MSIX_MAX_VECTORS / 64];
};

/* MSI-X Context */
struct msix_ctx {
    /* MSI-X Table (for user-space emulation) */
    struct msix_table_entry *table;
    u32 table_size;

    /* MSI-X PBA */
    struct msix_pba *pba;
    u32 pba_size;

    /* MSI-X State */
    bool enabled;
    bool function_mask;

    /* Vector states */
    bool masked[MSIX_MAX_VECTORS];
    bool pending[MSIX_MAX_VECTORS];
    u64 irq_count[MSIX_MAX_VECTORS];

    /* Lock for MSI-X operations */
    struct mutex lock;

    /* Parent device data */
    void *private_data;

    /* Interrupt callback (for user-space emulation) */
    void (*irq_callback)(void *data, u32 vector);
};

/* Function Prototypes */
int msix_init(struct msix_ctx *ctx, u32 num_vectors);
void msix_cleanup(struct msix_ctx *ctx);
int msix_enable(struct msix_ctx *ctx);
void msix_disable(struct msix_ctx *ctx);
int msix_set_function_mask(struct msix_ctx *ctx, bool mask);
int msix_mask_vector(struct msix_ctx *ctx, u32 vector);
int msix_unmask_vector(struct msix_ctx *ctx, u32 vector);
int msix_trigger_irq(struct msix_ctx *ctx, u32 vector);
int msix_table_read(struct msix_ctx *ctx, u32 offset, u64 *value, u32 size);
int msix_table_write(struct msix_ctx *ctx, u32 offset, u64 value, u32 size);
int msix_pba_read(struct msix_ctx *ctx, u32 offset, u64 *value, u32 size);
int msix_pba_write(struct msix_ctx *ctx, u32 offset, u64 value, u32 size);

#endif /* __HFSSS_PCIE_MSIX_H */
