#ifndef __HFSSS_HAL_H
#define __HFSSS_HAL_H

#include "common/common.h"
#include "common/mutex.h"
#include "hal/hal_nand.h"
#include "hal/hal_nor.h"
#include "hal/hal_pci.h"
#include "hal/hal_power.h"

/* HAL Statistics */
struct hal_stats {
    u64 nand_read_count;
    u64 nand_write_count;
    u64 nand_erase_count;
    u64 nand_read_bytes;
    u64 nand_write_bytes;
    u64 total_read_ns;
    u64 total_write_ns;
    u64 total_erase_ns;
    u64 nor_read_count;
    u64 nor_write_count;
    u64 nor_erase_count;
};

/* HAL Context */
struct hal_ctx {
    struct hal_nand_dev *nand;
    struct hal_nor_dev *nor;
    struct hal_pci_ctx *pci;
    struct hal_power_ctx *power;
    struct hal_stats stats;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
/* New full-featured hal_init */
int hal_init_full(struct hal_ctx *ctx, struct hal_nand_dev *nand_dev,
                  struct hal_nor_dev *nor_dev, struct hal_pci_ctx *pci_ctx,
                  struct hal_power_ctx *power_ctx);
/* Backward compatible hal_init (only takes nand_dev) */
int hal_init(struct hal_ctx *ctx, struct hal_nand_dev *nand_dev);
void hal_cleanup(struct hal_ctx *ctx);
int hal_nand_read_sync(struct hal_ctx *ctx, u32 ch, u32 chip, u32 die,
                        u32 plane, u32 block, u32 page, void *data, void *spare);
int hal_nand_program_sync(struct hal_ctx *ctx, u32 ch, u32 chip, u32 die,
                           u32 plane, u32 block, u32 page, const void *data, const void *spare);
int hal_nand_erase_sync(struct hal_ctx *ctx, u32 ch, u32 chip, u32 die,
                         u32 plane, u32 block);
int hal_ctx_nand_is_bad_block(struct hal_ctx *ctx, u32 ch, u32 chip,
                               u32 die, u32 plane, u32 block);
int hal_ctx_nand_mark_bad_block(struct hal_ctx *ctx, u32 ch, u32 chip,
                                 u32 die, u32 plane, u32 block);
u32 hal_ctx_nand_get_erase_count(struct hal_ctx *ctx, u32 ch, u32 chip,
                                   u32 die, u32 plane, u32 block);
/* NOR wrapper functions */
int hal_nor_read_sync(struct hal_ctx *ctx, u32 addr, void *data, u32 len);
int hal_nor_write_sync(struct hal_ctx *ctx, u32 addr, const void *data, u32 len);
int hal_nor_erase_sync(struct hal_ctx *ctx, u32 addr, u32 len);
/* PCI wrapper functions */
int hal_pci_submit_completion_sync(struct hal_ctx *ctx, const struct hal_pci_completion *comp);
int hal_pci_poll_completion_sync(struct hal_ctx *ctx, struct hal_pci_completion *comp);
int hal_pci_ns_attach_sync(struct hal_ctx *ctx, u32 nsid, u64 size, u32 lba_size);
int hal_pci_ns_detach_sync(struct hal_ctx *ctx, u32 nsid);
int hal_pci_ns_get_info_sync(struct hal_ctx *ctx, u32 nsid, struct hal_pci_namespace *info);
/* Power wrapper functions */
int hal_power_set_state_sync(struct hal_ctx *ctx, enum hal_nvme_power_state state);
enum hal_nvme_power_state hal_power_get_state_sync(struct hal_ctx *ctx);

void hal_get_stats(struct hal_ctx *ctx, struct hal_stats *stats);
void hal_reset_stats(struct hal_ctx *ctx);

#endif /* __HFSSS_HAL_H */
