# QoS Determinism Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-23 | HFSSS  | Initial release |

## Table of Contents

1. [Overview](#1-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [DWRR Scheduler Algorithm](#3-dwrr-scheduler-algorithm)
4. [Per-Namespace QoS Policy](#4-per-namespace-qos-policy)
5. [Latency SLA Monitor](#5-latency-sla-monitor)
6. [GC Impact Mitigation](#6-gc-impact-mitigation)
7. [OOB QoS Configuration Interface](#7-oob-qos-configuration-interface)
8. [Architecture Decision Records](#8-architecture-decision-records)
9. [Test Plan](#9-test-plan)

---

## 1. Overview

**Purpose**: Implement enterprise multi-tenant QoS determinism for the HFSSS simulator, ensuring that each namespace receives predictable performance guarantees regardless of other tenants' workloads.

**Scope**: This module covers the DWRR (Deficit Weighted Round Robin) command scheduler, per-namespace IOPS/bandwidth/latency policy enforcement, latency SLA monitoring with violation alerting, and GC impact mitigation strategies.

**References**: NVMe 2.0 Specification (QoS features, NVM Sets, Endurance Groups), JEDEC JESD219 (SSD Performance Test Method).

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-147 | DWRR scheduler: weighted fair queuing across namespaces | P0 | V3.0 |
| REQ-148 | Per-namespace QoS policy: IOPS limit, BW limit, latency target | P0 | V3.0 |
| REQ-149 | Token bucket rate limiting: per-NS IOPS and bandwidth enforcement | P0 | V3.0 |
| REQ-150 | Latency SLA monitor: per-NS histogram, P99/P99.9 tracking, violation alerts | P0 | V3.0 |
| REQ-151 | GC impact mitigation: preemption points, IO-priority-aware GC rate | P0 | V3.0 |
| REQ-152 | Deterministic windows: HOST_IO, GC_ALLOWED, GC_ONLY scheduling | P1 | V3.0 |
| REQ-153 | OOB QoS configuration: JSON-RPC methods for policy set/get/stats | P0 | V3.0 |

---

## 3. DWRR Scheduler Algorithm

### 3.1 Algorithm Description

The DWRR scheduler provides weighted fair queuing across namespaces. Each namespace queue has a weight W and a deficit counter DC. Per scheduling round:

```
for each active queue q:
    q.DC += quantum * q.weight
    while q.DC > 0 and q has pending commands:
        dispatch one command from q
        q.DC -= command_cost(cmd)
```

### 3.2 Data Structures

```c
struct dwrr_queue {
    uint32_t nsid;           /* namespace identifier */
    uint32_t weight;         /* scheduling weight (1-1000, default 100) */
    int32_t  deficit;        /* deficit counter (can go negative) */
    uint32_t pending_cmds;   /* number of commands waiting */
    struct cmd_list head;    /* linked list of pending commands */
    bool     active;         /* true if queue has pending work */
};

struct dwrr_scheduler {
    struct dwrr_queue queues[MAX_NAMESPACES];
    uint32_t active_count;   /* number of active queues */
    uint32_t base_quantum;   /* base quantum per round */
    uint32_t max_outstanding; /* max concurrent NAND commands */
    uint64_t tick_interval_ns; /* scheduler tick period (1ms default) */
    uint64_t last_tick_ns;
    pthread_mutex_t lock;
};
```

### 3.3 Quantum Calculation

```c
uint32_t dwrr_calculate_quantum(struct dwrr_scheduler *sched) {
    if (sched->active_count == 0) return 0;
    return sched->max_outstanding / sched->active_count;
}
```

### 3.4 Command Cost

```c
uint32_t dwrr_command_cost(const struct nvme_cmd *cmd) {
    /* Read: cost = 1 (low NAND occupancy) */
    /* Write: cost = 2 (NAND program time > read time) */
    /* Flush: cost = 4 (blocks pipeline) */
    switch (cmd->opcode) {
        case NVME_CMD_READ:  return 1;
        case NVME_CMD_WRITE: return 2;
        case NVME_CMD_FLUSH: return 4;
        default: return 1;
    }
}
```

### 3.5 Scheduler Tick

The scheduler is invoked on each command completion or periodically (every 1ms):

```c
void dwrr_schedule_tick(struct dwrr_scheduler *sched) {
    pthread_mutex_lock(&sched->lock);
    for (int i = 0; i < MAX_NAMESPACES; i++) {
        struct dwrr_queue *q = &sched->queues[i];
        if (!q->active) continue;

        q->deficit += sched->base_quantum * q->weight;
        while (q->deficit > 0 && q->pending_cmds > 0) {
            struct nvme_cmd *cmd = cmd_list_dequeue(&q->head);
            q->deficit -= dwrr_command_cost(cmd);
            q->pending_cmds--;
            dispatch_to_nand(cmd);
        }
        if (q->pending_cmds == 0)
            q->active = false;
    }
    pthread_mutex_unlock(&sched->lock);
}
```

---

## 4. Per-Namespace QoS Policy

### 4.1 Policy Data Structure

```c
struct ns_qos_policy {
    uint32_t nsid;
    uint32_t iops_limit;        /* max IOPS (0 = unlimited) */
    uint32_t bw_limit_mbps;     /* max bandwidth in MB/s (0 = unlimited) */
    uint32_t latency_target_us; /* target P99 latency in us (0 = no target) */
    uint32_t burst_allowance;   /* burst tokens above steady-state rate */
    bool     enforced;          /* true = limits actively enforced */
};
```

### 4.2 Token Bucket for IOPS

```c
struct token_bucket {
    uint64_t tokens;         /* current token count */
    uint64_t max_tokens;     /* burst ceiling */
    uint64_t refill_rate;    /* tokens per second */
    uint64_t last_refill_ns; /* timestamp of last refill */
};

/* IOPS bucket: 1 token = 1 I/O operation */
struct ns_qos_ctx {
    struct ns_qos_policy policy;
    struct token_bucket  iops_bucket;   /* token = 1 I/O */
    struct token_bucket  bw_bucket;     /* token = 1 byte */
    pthread_mutex_t      lock;
    pthread_cond_t       cond;          /* signaled on token refill */
};
```

### 4.3 Token Bucket for Bandwidth

Separate bucket with byte-based tokens. Each command consumes `transfer_size_bytes` tokens from the bandwidth bucket.

### 4.4 Enforcement

When a command arrives and tokens are exhausted:

```c
int qos_acquire_tokens(struct ns_qos_ctx *ctx, uint32_t io_count, uint64_t bytes) {
    pthread_mutex_lock(&ctx->lock);
    while (true) {
        qos_refill_tokens(ctx);
        if (ctx->iops_bucket.tokens >= io_count &&
            ctx->bw_bucket.tokens >= bytes) {
            ctx->iops_bucket.tokens -= io_count;
            ctx->bw_bucket.tokens -= bytes;
            pthread_mutex_unlock(&ctx->lock);
            return 0;
        }
        /* Wait for refill timer to add tokens */
        pthread_cond_wait(&ctx->cond, &ctx->lock);
    }
}
```

---

## 5. Latency SLA Monitor

### 5.1 Per-Namespace Latency Histogram

```c
#define LAT_HIST_BUCKETS 64  /* exponential: 1us, 2us, 4us, ... 2^63 us */

struct ns_latency_monitor {
    uint32_t nsid;
    uint64_t hist[LAT_HIST_BUCKETS];     /* bucket counts */
    uint64_t total_ops;
    uint64_t p99_us;                      /* cached P99 value */
    uint64_t p999_us;                     /* cached P99.9 value */
    uint32_t sla_violation_count;         /* P99 > target for 10 consecutive seconds */
    uint32_t consecutive_violation_secs;  /* current streak */
    uint64_t last_check_ns;
};
```

### 5.2 P99/P99.9 Calculation from Histogram

```c
uint64_t lat_hist_percentile(const uint64_t *hist, uint64_t total, double pct) {
    uint64_t target = (uint64_t)(total * pct);
    uint64_t cumsum = 0;
    for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
        cumsum += hist[i];
        if (cumsum >= target)
            return (i == 0) ? 0 : (1ULL << (i - 1));
    }
    return (1ULL << (LAT_HIST_BUCKETS - 1));
}
```

### 5.3 SLA Violation Detection

Checked every second by the monitoring thread:

```
if P99 > latency_target_us for this namespace:
    consecutive_violation_secs++
    if consecutive_violation_secs >= 10:
        sla_violation_count++
        Send alert via OOB notification (hal_aer_post_event)
        log WARN "SLA violation: NS %d P99=%llu us > target %llu us for %d seconds"
else:
    consecutive_violation_secs = 0
```

---

## 6. GC Impact Mitigation

### 6.1 GC Preemption Points

After each valid page copy during GC, the GC thread checks if host IO is waiting:

```c
void gc_copy_valid_page(struct gc_ctx *gc, uint64_t src_ppn, uint64_t dst_ppn) {
    nand_read_page(src_ppn, buf);
    nand_program_page(dst_ppn, buf);

    /* Preemption point: yield to host IO if waiting */
    if (dwrr_has_pending_host_io(gc->sched)) {
        gc_yield(gc);  /* sleep for gc_yield_us microseconds */
    }
}
```

### 6.2 IO-Priority-Aware GC

When host IO latency exceeds 2x baseline, GC rate is automatically reduced:

```c
void gc_adjust_rate(struct gc_ctx *gc, double current_p99_us, double baseline_p99_us) {
    double ratio = current_p99_us / baseline_p99_us;
    if (ratio > 2.0) {
        gc->rate_factor = max(0.1, gc->rate_factor * 0.8);  /* reduce by 20% */
        log_info("GC rate reduced to %.0f%% due to IO latency pressure", gc->rate_factor * 100);
    } else if (ratio < 1.2) {
        gc->rate_factor = min(1.0, gc->rate_factor * 1.1);  /* restore by 10% */
    }
}
```

### 6.3 Deterministic Windows

Three scheduling windows define when GC and host IO can execute:

| Window | Host IO | GC | Description |
|--------|---------|-----|-------------|
| HOST_IO | Allowed | Blocked | Pure host IO processing |
| GC_ALLOWED | Allowed | Allowed | Concurrent operation |
| GC_ONLY | Blocked | Allowed | Dedicated GC window |

### 6.4 Window Scheduling

```c
struct det_window_config {
    uint32_t host_io_pct;    /* e.g., 80% of cycle time */
    uint32_t gc_allowed_pct; /* e.g., 15% */
    uint32_t gc_only_pct;    /* e.g., 5% */
    uint32_t cycle_ms;       /* total cycle length (e.g., 1000ms) */
};
```

Example: 1-second cycle with 80/15/5 split:
- 0-800ms: HOST_IO window (no GC)
- 800-950ms: GC_ALLOWED window (concurrent)
- 950-1000ms: GC_ONLY window (no host IO)

---

## 7. OOB QoS Configuration Interface

### 7.1 JSON-RPC Methods

**qos.set_policy**:
```json
{
  "method": "qos.set_policy",
  "params": {
    "nsid": 1,
    "iops_limit": 100000,
    "bw_limit_mbps": 500,
    "latency_target_us": 200
  }
}
```

**qos.get_policy**:
```json
{
  "method": "qos.get_policy",
  "params": { "nsid": 1 }
}
```
Response includes all policy fields plus current enforcement status.

**qos.get_stats**:
```json
{
  "method": "qos.get_stats",
  "params": { "nsid": 1 }
}
```
Response:
```json
{
  "nsid": 1,
  "current_iops": 95000,
  "current_bw_mbps": 475,
  "p99_us": 180,
  "p999_us": 450,
  "sla_violations": 0,
  "iops_tokens_remaining": 5000,
  "bw_tokens_remaining_mb": 25,
  "gc_preemptions": 142
}
```

**qos.reset_stats**:
```json
{
  "method": "qos.reset_stats",
  "params": { "nsid": 1 }
}
```

---

## 8. Architecture Decision Records

### ADR-001: DWRR over WFQ

**Context**: Multiple fair queuing algorithms exist (WFQ, WRR, DRR, DWRR).

**Decision**: Use DWRR because it provides O(1) per-packet scheduling complexity (no sorting), supports variable-cost commands, and is simple to implement correctly.

**Rationale**: WFQ (Weighted Fair Queuing) requires maintaining a sorted virtual time structure with O(log N) complexity. DWRR achieves similar fairness with much simpler implementation, which is more suitable for simulation where correctness is prioritized over perfect fairness.

### ADR-002: Token Bucket over Leaky Bucket

**Context**: Rate limiting can be implemented via token bucket (allows bursts) or leaky bucket (strict rate).

**Decision**: Use token bucket with configurable burst allowance. This better matches real SSD behavior where short bursts are acceptable as long as sustained rate stays within limits.

**Rationale**: Enterprise workloads are inherently bursty. A strict leaky bucket would over-penalize normal burst patterns and produce unrealistic latency behavior.

### ADR-003: Deterministic Windows for GC Isolation

**Context**: GC can cause unpredictable latency spikes. Options: (a) GC preemption only, (b) deterministic time windows, (c) both.

**Decision**: Implement both GC preemption points and deterministic windows. Preemption provides fine-grained responsiveness; windows provide macro-level GC budget control.

**Rationale**: Enterprise SSDs (e.g., Samsung PM9A3) use deterministic windows for predictable performance. Combining both approaches gives the highest QoS determinism.

---

## 9. Test Plan

| Test ID | Description | Verification Point |
|---------|-------------|-------------------|
| QD-001 | DWRR equal weight: 2 namespaces | Each gets ~50% of IOPS |
| QD-002 | DWRR unequal weight: NS1=100, NS2=300 | NS2 gets ~3x NS1's IOPS |
| QD-003 | DWRR single active namespace | Gets 100% of capacity |
| QD-004 | DWRR command cost: write costs 2x read | Writes get proportionally fewer slots |
| QD-005 | IOPS token bucket: limit 100K IOPS | Sustained rate <= 100K +/- 5% |
| QD-006 | IOPS burst: 10K burst allowance | Short bursts up to 110K accepted |
| QD-007 | BW token bucket: limit 500 MB/s | Sustained BW <= 500 MB/s |
| QD-008 | Token exhaustion: command held in wait queue | Command blocked until refill |
| QD-009 | Multi-NS isolation: NS1 overloaded, NS2 normal | NS2 latency unaffected |
| QD-010 | P99 latency histogram accuracy | Injected fixed latency falls in correct bucket |
| QD-011 | P99.9 calculation correctness | Matches expected percentile value |
| QD-012 | SLA violation: P99 > target for 10 seconds | sla_violation_count incremented; alert sent |
| QD-013 | SLA recovery: P99 drops below target | consecutive_violation_secs resets |
| QD-014 | GC preemption: host IO waiting during GC | GC yields; host IO latency < 2x baseline |
| QD-015 | GC rate reduction: high latency pressure | gc_rate_factor reduced; latency improves |
| QD-016 | Deterministic window: HOST_IO phase | No GC activity during HOST_IO window |
| QD-017 | Deterministic window: GC_ONLY phase | No host IO accepted during GC_ONLY window |
| QD-018 | qos.set_policy JSON-RPC | Policy applied; get_policy returns new values |
| QD-019 | qos.get_stats JSON-RPC | Stats match observed behavior |
| QD-020 | qos.reset_stats JSON-RPC | All counters zeroed |
| QD-021 | Thermal throttle integration | throttle_factor applied to DWRR quantum |
| QD-022 | 72-hour multi-tenant stability | No SLA violations under normal load |

---

**Document Statistics**:
- Requirements covered: 7 (REQ-147 through REQ-153)
- New header files: `include/controller/qos.h`
- New source files: `src/controller/dwrr_scheduler.c`, `src/controller/qos_policy.c`, `src/controller/latency_monitor.c`, `src/controller/gc_mitigation.c`
- Function interfaces: 30+
- Test cases: 22

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| OOB JSON-RPC framework | LLD_07_OOB_MANAGEMENT |
| Thermal throttle integration | LLD_12_REALTIME_SERVICES |
| FTL flow control | LLD_11_FTL_RELIABILITY |
| GC algorithm | LLD_06_FTL (not in scope) |
| Performance validation | LLD_10_PERFORMANCE_VALIDATION |
