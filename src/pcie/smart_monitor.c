/*
 * REQ-178: SMART monitor runtime producer. See smart_monitor.h for
 * the threshold semantics that drive AER emission.
 */

#include "pcie/smart_monitor.h"
#include "pcie/nvme_uspace.h"
#include "common/thermal.h"
#include <string.h>
#include <time.h>
#include <errno.h>

static u8 clamp100(u8 v) { return v > 100 ? 100 : v; }

int smart_monitor_init(struct smart_monitor *mon,
                       const struct smart_monitor_config *cfg)
{
    if (!mon || !cfg || !cfg->dev ||
        !cfg->get_thermal || !cfg->get_remaining_life ||
        !cfg->get_spare) {
        return HFSSS_ERR_INVAL;
    }
    memset(mon, 0, sizeof(*mon));
    mon->cfg = *cfg;
    atomic_store(&mon->stop, false);

    /* Seed previous values to the nominal baseline (level=0, worn
     * 0%, spare 100%). A device that enumerates already-degraded
     * will differ on the first poll and correctly fire the AER —
     * previously a `have_baseline` latch suppressed the first event
     * and masked degraded-at-start conditions from the host. */
    mon->last_thermal      = 0;
    mon->last_wear_bucket  = 0;
    mon->last_spare_bucket = 10;

    mon->initialized = true;
    return HFSSS_OK;
}

void smart_monitor_poll_once(struct smart_monitor *mon)
{
    if (!mon || !mon->initialized) {
        return;
    }
    const struct smart_monitor_config *cfg = &mon->cfg;

    u8 thermal = cfg->get_thermal(cfg->cb_ctx);
    u8 rlp     = clamp100(cfg->get_remaining_life(cfg->cb_ctx));
    u8 spare   = clamp100(cfg->get_spare(cfg->cb_ctx));

    /* Derive buckets so same-bucket fluctuations don't fire AERs. */
    u8 wear_bucket  = (u8)((100 - rlp) / 10);
    u8 spare_bucket = (u8)(spare / 10);

    /* Thermal: any level change (up or down) fires. Cool-down is
     * interesting too because the host may want to drop throttling. */
    if (thermal != mon->last_thermal) {
        bool delivered = false;
        int rc = nvme_uspace_aer_notify_thermal(cfg->dev, thermal,
                                                &delivered, NULL, NULL);
        if (rc == HFSSS_OK) {
            mon->notify_count_thermal++;
        }
        mon->last_thermal = thermal;
    }

    /* Wear: we only care about increases. Bucket boundary crossings
     * at 90%/80%/70%/... of remaining life are the interesting
     * host-visible events. */
    if (wear_bucket > mon->last_wear_bucket) {
        bool delivered = false;
        int rc = nvme_uspace_aer_notify_wear(cfg->dev, rlp,
                                             &delivered, NULL, NULL);
        if (rc == HFSSS_OK) {
            mon->notify_count_wear++;
        }
        mon->last_wear_bucket = wear_bucket;
    }

    /* Spare: we care about decreases. Lower bucket == less spare
     * available, which is the threshold condition NVMe §5.14.1.2
     * bit 0 models. */
    if (spare_bucket < mon->last_spare_bucket) {
        bool delivered = false;
        int rc = nvme_uspace_aer_notify_spare(cfg->dev, spare,
                                              &delivered, NULL, NULL);
        if (rc == HFSSS_OK) {
            mon->notify_count_spare++;
        }
        mon->last_spare_bucket = spare_bucket;
    }
}

static void monitor_sleep_ms(u32 ms)
{
    if (ms == 0) {
        /* Yield briefly so a 0-ms interval doesn't pin a core while
         * the thread spins. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        return;
    }
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000 * 1000;
    nanosleep(&ts, NULL);
}

static void *monitor_thread(void *arg)
{
    struct smart_monitor *mon = (struct smart_monitor *)arg;
    while (!atomic_load(&mon->stop)) {
        smart_monitor_poll_once(mon);
        monitor_sleep_ms(mon->cfg.poll_interval_ms);
    }
    return NULL;
}

int smart_monitor_start(struct smart_monitor *mon)
{
    if (!mon || !mon->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (mon->running) {
        return HFSSS_OK;  /* idempotent */
    }
    atomic_store(&mon->stop, false);
    int rc = pthread_create(&mon->thread, NULL, monitor_thread, mon);
    if (rc != 0) {
        return HFSSS_ERR_IO;
    }
    mon->running = true;
    return HFSSS_OK;
}

void smart_monitor_stop(struct smart_monitor *mon)
{
    if (!mon || !mon->running) {
        return;
    }
    atomic_store(&mon->stop, true);
    pthread_join(mon->thread, NULL);
    mon->running = false;
}

void smart_monitor_cleanup(struct smart_monitor *mon)
{
    if (!mon) {
        return;
    }
    if (mon->running) {
        smart_monitor_stop(mon);
    }
    memset(mon, 0, sizeof(*mon));
}
