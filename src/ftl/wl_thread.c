#include "ftl/ftl_worker.h"
#include <stdio.h>
#include <unistd.h>

#define WL_INTERVAL_SEC  10
#define WL_THRESHOLD     100   /* Max erase count delta before leveling */

void *wl_thread_main(void *arg)
{
    struct ftl_mt_ctx *mt = (struct ftl_mt_ctx *)arg;
    struct ftl_ctx *ftl = &mt->ftl;

    while (mt->wl_running) {
        sleep(WL_INTERVAL_SEC);

        if (!mt->wl_running) break;

        /* Wear leveling: check erase count spread */
        wear_level_check_wear(&ftl->wl, &ftl->block_mgr, WL_THRESHOLD);
    }

    return NULL;
}
