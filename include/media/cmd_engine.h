#ifndef __HFSSS_NAND_CMD_ENGINE_H
#define __HFSSS_NAND_CMD_ENGINE_H

#include "common/common.h"
#include "media/cmd_state.h"
#include "media/nand_identity.h"

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
 * Status submission paths. Both take only the die-level lock so they remain
 * observable mid-flight; neither touches the channel lock or the cmd_state
 * begin/advance path.
 */
int nand_cmd_engine_submit_read_status(struct nand_device *dev, const struct nand_cmd_target *target,
                                       u8 *out_status_byte);

int nand_cmd_engine_submit_read_status_enhanced(struct nand_device *dev, const struct nand_cmd_target *target,
                                                struct nand_status_enhanced *out);

/*
 * Identity submission paths. Both take the full channel+die lock order and
 * are legal only from DIE_IDLE or the suspend states (non-conflicting reads).
 * Current phase skips EAT advancement for these bus-bound operations — see
 * engine_submit_identity in cmd_engine.c.
 */
int nand_cmd_engine_submit_read_id(struct nand_device *dev, const struct nand_cmd_target *target, struct nand_id *out);

int nand_cmd_engine_submit_read_param_page(struct nand_device *dev, const struct nand_cmd_target *target,
                                           struct nand_parameter_page *out);

/*
 * Suspend/resume submission paths. All four take only the die-level lock
 * so the submitting thread can reach the running worker without blocking
 * on the channel lock that the worker released across its array-busy
 * spin. The suspend entry points only use (ch, chip, die) from target;
 * block/page/plane_mask fields are ignored because these commands act on
 * the currently in-flight op of the addressed die.
 */
int nand_cmd_engine_submit_prog_suspend(struct nand_device *dev, const struct nand_cmd_target *target);
int nand_cmd_engine_submit_prog_resume(struct nand_device *dev, const struct nand_cmd_target *target);
int nand_cmd_engine_submit_erase_suspend(struct nand_device *dev, const struct nand_cmd_target *target);
int nand_cmd_engine_submit_erase_resume(struct nand_device *dev, const struct nand_cmd_target *target);

int nand_cmd_engine_submit_cache_read(struct nand_device *dev, const struct nand_cmd_target *target,
                                      const struct nand_cmd_ops *ops, void *cb_ctx);
int nand_cmd_engine_submit_cache_read_end(struct nand_device *dev, const struct nand_cmd_target *target,
                                          const struct nand_cmd_ops *ops, void *cb_ctx);
int nand_cmd_engine_submit_cache_program(struct nand_device *dev, const struct nand_cmd_target *target,
                                         const struct nand_cmd_ops *ops, void *cb_ctx);

/*
 * Stage-budget split. Kept in its own translation unit so the partition
 * can be refined later without touching the engine.
 */
void nand_cmd_stage_budget(enum nand_cmd_opcode op, u64 total_ns, u64 cache_overlap_ns, u64 *setup_ns, u64 *array_ns,
                           u64 *xfer_ns);

#endif /* __HFSSS_NAND_CMD_ENGINE_H */
