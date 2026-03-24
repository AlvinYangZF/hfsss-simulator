#include "controller/qos.h"
#include <stdlib.h>
#include <string.h>

int det_window_init(struct det_window_config *cfg, u32 host_pct,
                    u32 gc_allowed_pct, u32 gc_only_pct, u32 cycle_ms)
{
    if (!cfg) {
        return HFSSS_ERR_INVAL;
    }

    if (host_pct + gc_allowed_pct + gc_only_pct != 100) {
        return HFSSS_ERR_INVAL;
    }

    if (cycle_ms == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->host_io_pct = host_pct;
    cfg->gc_allowed_pct = gc_allowed_pct;
    cfg->gc_only_pct = gc_only_pct;
    cfg->cycle_ms = cycle_ms;
    cfg->cycle_start_ns = get_time_ns();
    cfg->enabled = true;

    return HFSSS_OK;
}

enum det_window_phase det_window_get_phase(const struct det_window_config *cfg,
                                           u64 now_ns)
{
    if (!cfg || !cfg->enabled) {
        return DW_HOST_IO;
    }

    u64 cycle_ns = (u64)cfg->cycle_ms * 1000000ULL;
    if (cycle_ns == 0) {
        return DW_HOST_IO;
    }

    /* Position within the current cycle (0 to cycle_ns-1) */
    u64 elapsed = now_ns - cfg->cycle_start_ns;
    u64 pos = elapsed % cycle_ns;

    /* Convert position to per-mille of cycle for comparison */
    u64 host_end_ns = (cycle_ns * cfg->host_io_pct) / 100;
    u64 gc_allowed_end_ns = host_end_ns +
                            (cycle_ns * cfg->gc_allowed_pct) / 100;

    if (pos < host_end_ns) {
        return DW_HOST_IO;
    } else if (pos < gc_allowed_end_ns) {
        return DW_GC_ALLOWED;
    } else {
        return DW_GC_ONLY;
    }
}

bool det_window_allow_gc(const struct det_window_config *cfg, u64 now_ns)
{
    if (!cfg || !cfg->enabled) {
        return true;  /* allow GC if not configured */
    }

    enum det_window_phase phase = det_window_get_phase(cfg, now_ns);
    return (phase == DW_GC_ALLOWED || phase == DW_GC_ONLY);
}

bool det_window_allow_host_io(const struct det_window_config *cfg, u64 now_ns)
{
    if (!cfg || !cfg->enabled) {
        return true;  /* allow host IO if not configured */
    }

    enum det_window_phase phase = det_window_get_phase(cfg, now_ns);
    return (phase == DW_HOST_IO || phase == DW_GC_ALLOWED);
}
