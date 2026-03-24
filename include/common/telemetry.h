#ifndef __HFSSS_TELEMETRY_H
#define __HFSSS_TELEMETRY_H

#include "common/common.h"

/*
 * Telemetry and SMART Prediction Module (REQ-175 through REQ-178)
 *
 * Provides a ring-buffer telemetry event log and SMART remaining
 * life prediction based on erase counts and write amplification.
 */

#define TEL_MAX_EVENTS  4096
#define TEL_PAYLOAD_LEN 32

/* Telemetry event types */
enum tel_event_type {
    TEL_EVENT_THERMAL    = 1,
    TEL_EVENT_GC         = 2,
    TEL_EVENT_ERROR      = 3,
    TEL_EVENT_SLA_VIOL   = 4,
    TEL_EVENT_POWER      = 5,
    TEL_EVENT_WEAR       = 6,
    TEL_EVENT_SPARE      = 7,
};

/* Telemetry event */
struct tel_event {
    u64 timestamp_ns;
    u32 type;           /* enum tel_event_type */
    u8  severity;       /* 0=info, 1=warn, 2=error, 3=critical */
    u8  reserved[3];
    u8  payload[TEL_PAYLOAD_LEN];
};

/* Telemetry ring buffer */
struct telemetry_log {
    struct tel_event events[TEL_MAX_EVENTS];
    u32 head;
    u32 count;
    u64 total_events;
    bool initialized;
};

/* SMART remaining life prediction */
struct smart_prediction {
    double remaining_life_pct;  /* 0.0-100.0 */
    u64 estimated_remaining_writes_tb;
    double current_waf;
    u32 avg_erase_count;
    u32 max_pe_cycles;
};

/* Initialize telemetry log */
int telemetry_init(struct telemetry_log *log);

/* Clean up telemetry log */
void telemetry_cleanup(struct telemetry_log *log);

/* Record a telemetry event into the ring buffer */
int telemetry_record(struct telemetry_log *log, enum tel_event_type type,
                     u8 severity, const void *payload, u32 payload_len);

/* Retrieve most recent events from the ring buffer */
int telemetry_get_recent(const struct telemetry_log *log,
                         struct tel_event *out, u32 max_events,
                         u32 *actual);

/* Get total number of events ever recorded */
u64 telemetry_get_total_count(const struct telemetry_log *log);

/* Predict remaining SMART life based on erase counts and WAF */
void smart_predict_life(u32 avg_erase_count, u32 max_pe_cycles,
                        double waf, struct smart_prediction *pred);

#endif /* __HFSSS_TELEMETRY_H */
