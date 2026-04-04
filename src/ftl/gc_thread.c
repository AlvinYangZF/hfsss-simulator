#include "ftl/ftl_worker.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void *gc_thread_main(void *arg)
{
    struct ftl_mt_ctx *mt = (struct ftl_mt_ctx *)arg;
    struct ftl_ctx *ftl = &mt->ftl;

    while (mt->gc_running) {
        /* Wait for signal from FTL workers or periodic timeout */
        pthread_mutex_lock(&mt->gc_mutex);
        while (mt->gc_running &&
               !gc_should_trigger(&ftl->gc, ftl->block_mgr.free_blocks)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  /* Check every 1 second even without signal */
            pthread_cond_timedwait(&mt->gc_cond, &mt->gc_mutex, &ts);
        }
        pthread_mutex_unlock(&mt->gc_mutex);

        if (!mt->gc_running) break;

        /* Run GC cycle */
        int rc = gc_run(&ftl->gc, &ftl->block_mgr, &ftl->mapping,
                         ftl->hal);
        if (rc == HFSSS_OK) {
            ftl->stats.gc_count++;
        }
    }

    return NULL;
}
