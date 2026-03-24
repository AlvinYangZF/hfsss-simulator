#include "common/telemetry.h"
#include <string.h>

/*
 * Telemetry and SMART Prediction Implementation (REQ-175 through REQ-178)
 *
 * Ring-buffer telemetry log that overwrites oldest events when full.
 * SMART remaining life prediction based on average erase count relative
 * to maximum PE cycle rating and write amplification factor.
 */

int telemetry_init(struct telemetry_log *log) {
    if (!log) {
        return HFSSS_ERR_INVAL;
    }

    memset(log, 0, sizeof(*log));
    log->head = 0;
    log->count = 0;
    log->total_events = 0;
    log->initialized = true;

    return HFSSS_OK;
}

void telemetry_cleanup(struct telemetry_log *log) {
    if (!log || !log->initialized) {
        return;
    }
    log->initialized = false;
}

int telemetry_record(struct telemetry_log *log, enum tel_event_type type,
                     u8 severity, const void *payload, u32 payload_len) {
    if (!log || !log->initialized) {
        return HFSSS_ERR_INVAL;
    }

    /* Write event at head position */
    struct tel_event *ev = &log->events[log->head];
    memset(ev, 0, sizeof(*ev));

    ev->timestamp_ns = get_time_ns();
    ev->type = (u32)type;
    ev->severity = severity;

    if (payload && payload_len > 0) {
        u32 copy_len = (payload_len > TEL_PAYLOAD_LEN)
                       ? TEL_PAYLOAD_LEN : payload_len;
        memcpy(ev->payload, payload, copy_len);
    }

    /* Advance head (ring buffer wrap) */
    log->head = (log->head + 1) % TEL_MAX_EVENTS;

    if (log->count < TEL_MAX_EVENTS) {
        log->count++;
    }
    log->total_events++;

    return HFSSS_OK;
}

int telemetry_get_recent(const struct telemetry_log *log,
                         struct tel_event *out, u32 max_events,
                         u32 *actual) {
    if (!log || !log->initialized || !out || !actual) {
        return HFSSS_ERR_INVAL;
    }

    u32 to_copy = (max_events < log->count) ? max_events : log->count;
    *actual = to_copy;

    if (to_copy == 0) {
        return HFSSS_OK;
    }

    /*
     * Events are stored with head pointing to the next write slot.
     * The most recent event is at (head - 1), the second most recent
     * at (head - 2), etc. We return them in reverse chronological
     * order (most recent first).
     */
    for (u32 i = 0; i < to_copy; i++) {
        u32 idx = (log->head + TEL_MAX_EVENTS - 1 - i) % TEL_MAX_EVENTS;
        out[i] = log->events[idx];
    }

    return HFSSS_OK;
}

u64 telemetry_get_total_count(const struct telemetry_log *log) {
    if (!log || !log->initialized) {
        return 0;
    }
    return log->total_events;
}

void smart_predict_life(u32 avg_erase_count, u32 max_pe_cycles,
                        double waf, struct smart_prediction *pred) {
    if (!pred) {
        return;
    }

    memset(pred, 0, sizeof(*pred));
    pred->avg_erase_count = avg_erase_count;
    pred->max_pe_cycles = max_pe_cycles;
    pred->current_waf = waf;

    /* Handle edge case: 0 max PE cycles (avoid division by zero) */
    if (max_pe_cycles == 0) {
        pred->remaining_life_pct = 0.0;
        pred->estimated_remaining_writes_tb = 0;
        return;
    }

    /* Remaining life percentage */
    double used_ratio = (double)avg_erase_count / (double)max_pe_cycles;
    if (used_ratio > 1.0) {
        used_ratio = 1.0;
    }
    pred->remaining_life_pct = 100.0 * (1.0 - used_ratio);

    /*
     * Estimate remaining TB of host writes.
     * Remaining PE cycles = max_pe_cycles - avg_erase_count
     * Each PE cycle writes one block worth of data to NAND.
     * Assuming 256 pages/block * 16KB/page = 4MB per block.
     * With WAF, host bytes per PE cycle = 4MB / WAF.
     * Total remaining host TB = remaining_cycles * 4MB / WAF / 1TB
     *
     * Use a default WAF of 1.0 if waf is zero or negative.
     */
    double effective_waf = (waf > 0.0) ? waf : 1.0;
    u32 remaining_cycles = 0;
    if (avg_erase_count < max_pe_cycles) {
        remaining_cycles = max_pe_cycles - avg_erase_count;
    }

    /* 4MB per block, convert to TB: 4MB = 4 * 1024 * 1024 bytes
     * 1 TB = 1099511627776 bytes
     * remaining_tb = remaining_cycles * 4194304 / effective_waf / 1099511627776
     *              = remaining_cycles / effective_waf / 262144 */
    double remaining_tb = (double)remaining_cycles / effective_waf / 262144.0;
    pred->estimated_remaining_writes_tb = (u64)remaining_tb;
}
