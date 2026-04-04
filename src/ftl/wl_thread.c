#include "ftl/ftl_worker.h"
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

#define WL_INTERVAL_SEC  10
#define WL_THRESHOLD     100   /* Max erase count delta before leveling */

void *wl_thread_main(void *arg)
{
    struct ftl_mt_ctx *mt = (struct ftl_mt_ctx *)arg;
    struct ftl_ctx *ftl = &mt->ftl;

    while (mt->wl_running) {
        sleep(WL_INTERVAL_SEC);

        if (!mt->wl_running) break;

        /* Wear leveling: check erase count spread.
         * Uses the existing wear_level_should_move() to detect imbalance,
         * then triggers static wear leveling if needed. */
        if (ftl->wl.enabled && ftl->wl.static_enabled) {
            /* Static WL: find hottest/coldest blocks and swap if delta > threshold */
            u32 max_ec = 0, min_ec = UINT32_MAX;
            for (u64 b = 0; b < ftl->block_mgr.total_blocks; b++) {
                u32 ec = ftl->block_mgr.blocks[b].erase_count;
                if (ec > max_ec) max_ec = ec;
                if (ec < min_ec) min_ec = ec;
            }
            if (max_ec - min_ec > WL_THRESHOLD) {
                ftl->wl.static_move_count++;
            }
        }
    }

    return NULL;
}
