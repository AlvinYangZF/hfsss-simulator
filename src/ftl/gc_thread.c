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
               !gc_should_trigger(&ftl->gc,
                                  block_get_free_count(&ftl->block_mgr))) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  /* Check every 1 second even without signal */
            pthread_cond_timedwait(&mt->gc_cond, &mt->gc_mutex, &ts);
        }
        pthread_mutex_unlock(&mt->gc_mutex);

        if (!mt->gc_running) break;

        /*
         * Reaching this point means gc_should_trigger() was true: the
         * free-block count crossed the GC threshold. The kick site in
         * ftl_write_page_mt_ex pre-tags the trigger before signaling
         * the worker; honour that tag if it is already set to a more
         * urgent class. Default any unset path (e.g. periodic timeout
         * that found free-low on its own) to FREE_SB_LOW since that is
         * the predicate that just succeeded.
         */
        gc_set_trigger(&ftl->gc, GC_TRIGGER_FREE_SB_LOW);

        /* Run GC cycle */
        int rc = gc_run_mt(&ftl->gc, &ftl->block_mgr, &mt->taa,
                            ftl->hal);
        if (rc == HFSSS_OK) {
            ftl->stats.gc_count++;
        }
    }

    return NULL;
}
