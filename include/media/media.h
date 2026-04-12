#ifndef __HFSSS_MEDIA_H
#define __HFSSS_MEDIA_H

#include "common/common.h"
#include "common/mutex.h"
#include "media/nand.h"
#include "media/nand_identity.h"
#include "media/timing.h"
#include "media/eat.h"
#include "media/reliability.h"
#include "media/bbt.h"

#define HFSSS_ERR_IO (-100)

/* Media Configuration */
struct media_config {
    u32 channel_count;
    u32 chips_per_channel;
    u32 dies_per_chip;
    u32 planes_per_die;
    u32 blocks_per_plane;
    u32 pages_per_block;
    u32 page_size;
    u32 spare_size;
    enum nand_type nand_type;
    bool enable_multi_plane;
    bool enable_die_interleaving;
};

/* Media Statistics */
struct media_stats {
    u64 read_count;
    u64 write_count;
    u64 erase_count;
    u64 total_read_bytes;
    u64 total_write_bytes;
    u64 total_read_ns;
    u64 total_write_ns;
    u64 total_erase_ns;
};

/* Media Context */
struct media_ctx {
    struct media_config config;
    struct nand_device *nand;
    struct timing_model *timing;
    struct eat_ctx *eat;
    struct reliability_model *reliability;
    struct bbt *bbt;
    struct media_stats stats;
    struct mutex lock;
    bool initialized;
};

/* Function Prototypes */
int media_init(struct media_ctx *ctx, struct media_config *config);
void media_cleanup(struct media_ctx *ctx);
int media_nand_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page, void *data,
                    void *spare);
int media_nand_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page,
                       const void *data, const void *spare);
int media_nand_erase(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
int media_nand_read_status(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_die_cmd_state *out);
int media_nand_read_status_byte(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u8 *out);
int media_nand_read_status_enhanced(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_status_enhanced *out);
int media_nand_read_id(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_id *out);
int media_nand_read_parameter_page(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, struct nand_parameter_page *out);
int media_nand_multi_plane_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane_mask, u32 block, u32 page,
                                void **data_array, void **spare_array);
int media_nand_multi_plane_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane_mask, u32 block,
                                   u32 page, const void **data_array, const void **spare_array);
int media_nand_multi_plane_erase(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane_mask, u32 block);
int media_nand_cache_read(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page, void *data,
                          void *spare);
int media_nand_cache_read_end(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page,
                              void *data, void *spare);
int media_nand_cache_program(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block, u32 page,
                             const void *data, const void *spare);
int media_nand_program_suspend(struct media_ctx *ctx, u32 ch, u32 chip, u32 die);
int media_nand_program_resume(struct media_ctx *ctx, u32 ch, u32 chip, u32 die);
int media_nand_erase_suspend(struct media_ctx *ctx, u32 ch, u32 chip, u32 die);
int media_nand_erase_resume(struct media_ctx *ctx, u32 ch, u32 chip, u32 die);
int media_nand_reset(struct media_ctx *ctx, u32 ch, u32 chip, u32 die);
int media_nand_is_bad_block(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
int media_nand_mark_bad_block(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
u32 media_nand_get_erase_count(struct media_ctx *ctx, u32 ch, u32 chip, u32 die, u32 plane, u32 block);
void media_get_stats(struct media_ctx *ctx, struct media_stats *stats);
void media_reset_stats(struct media_ctx *ctx);

/* Persistence functions */
int media_save(struct media_ctx *ctx, const char *filepath);
int media_save_incremental(struct media_ctx *ctx, const char *filepath);
int media_load(struct media_ctx *ctx, const char *filepath);
int media_create_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir);
int media_create_incremental_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir);
int media_restore_checkpoint(struct media_ctx *ctx, const char *checkpoint_dir);
int media_has_dirty_data(struct media_ctx *ctx);
void media_mark_all_clean(struct media_ctx *ctx);

#endif /* __HFSSS_MEDIA_H */
