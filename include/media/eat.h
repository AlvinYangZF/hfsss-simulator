#ifndef __HFSSS_EAT_H
#define __HFSSS_EAT_H

#include "common/common.h"

#define MAX_CHANNELS 64
#define MAX_CHIPS_PER_CHANNEL 16
#define MAX_DIES_PER_CHIP 8
#define MAX_PLANES_PER_DIE 4

/* Operation Type */
enum op_type {
    OP_READ = 0,
    OP_PROGRAM = 1,
    OP_ERASE = 2,
};

/* EAT Context */
struct eat_ctx {
    u64 channel_eat[MAX_CHANNELS];
    u64 chip_eat[MAX_CHANNELS][MAX_CHIPS_PER_CHANNEL];
    u64 die_eat[MAX_CHANNELS][MAX_CHIPS_PER_CHANNEL][MAX_DIES_PER_CHIP];
    u64 plane_eat[MAX_CHANNELS][MAX_CHIPS_PER_CHANNEL][MAX_DIES_PER_CHIP][MAX_PLANES_PER_DIE];
};

/* Function Prototypes */
int eat_ctx_init(struct eat_ctx *ctx);
void eat_ctx_cleanup(struct eat_ctx *ctx);
u64 eat_get_for_plane(struct eat_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane);
u64 eat_get_for_die(struct eat_ctx *ctx, u32 ch, u32 chip, u32 die);
u64 eat_get_for_chip(struct eat_ctx *ctx, u32 ch, u32 chip);
u64 eat_get_for_channel(struct eat_ctx *ctx, u32 ch);
u64 eat_get_max(struct eat_ctx *ctx, enum op_type op, u32 ch, u32 chip, u32 die, u32 plane);
void eat_update(struct eat_ctx *ctx, enum op_type op, u32 ch, u32 chip, u32 die, u32 plane, u64 duration);
void eat_update_stage(struct eat_ctx *ctx, enum op_type op, u32 ch, u32 chip, u32 die, u32 plane, u64 stage_duration);
void eat_reset(struct eat_ctx *ctx);

#endif /* __HFSSS_EAT_H */
