#ifndef __HFSSS_NAND_CMD_ENGINE_H
#define __HFSSS_NAND_CMD_ENGINE_H

#include "common/common.h"
#include "media/cmd_state.h"

struct nand_device;
struct eat_ctx;
struct timing_model;

/*
 * Per-phase commit callbacks. Any field may be NULL; a NULL slot is treated
 * as a no-op success. A non-OK return cancels any remaining phases but the
 * engine still drives the die back to DIE_IDLE so no die gets stuck.
 */
struct nand_cmd_ops {
    int (*on_setup_commit)(void *ctx);
    int (*on_array_commit)(void *ctx);
    int (*on_data_xfer_commit)(void *ctx);
};

int nand_cmd_engine_init(struct nand_device *dev, struct eat_ctx *eat, struct timing_model *timing);
void nand_cmd_engine_cleanup(struct nand_device *dev);

int nand_cmd_engine_submit_read(struct nand_device *dev, const struct nand_cmd_target *target,
                                const struct nand_cmd_ops *ops, void *cb_ctx);

int nand_cmd_engine_submit_program(struct nand_device *dev, const struct nand_cmd_target *target,
                                   const struct nand_cmd_ops *ops, void *cb_ctx);

int nand_cmd_engine_submit_erase(struct nand_device *dev, const struct nand_cmd_target *target,
                                 const struct nand_cmd_ops *ops, void *cb_ctx);

int nand_cmd_engine_submit_reset(struct nand_device *dev, const struct nand_cmd_target *target);

int nand_cmd_engine_snapshot(struct nand_device *dev, const struct nand_cmd_target *target,
                             struct nand_die_cmd_state *out);

/*
 * Stage-budget split. Kept in its own translation unit so the partition
 * can be refined later without touching the engine.
 */
void nand_cmd_stage_budget(enum nand_cmd_opcode op, u64 total_ns, u64 *setup_ns, u64 *array_ns, u64 *xfer_ns);

#endif /* __HFSSS_NAND_CMD_ENGINE_H */
