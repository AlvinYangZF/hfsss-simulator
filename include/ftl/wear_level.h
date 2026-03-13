#ifndef __HFSSS_WEAR_LEVEL_H
#define __HFSSS_WEAR_LEVEL_H

#include "common/common.h"

/* Wear Leveling Context (Placeholder) */
struct wear_level_ctx {
    u32 enabled;
    u64 move_count;
};

/* Function Prototypes (Placeholders) */
int wear_level_init(struct wear_level_ctx *ctx);
void wear_level_cleanup(struct wear_level_ctx *ctx);

#endif /* __HFSSS_WEAR_LEVEL_H */
