/*
 * ns_gc.c - Multi-namespace GC coordinator (REQ-166 through REQ-170)
 *
 * Coordinates garbage collection across multiple namespaces using urgency-based
 * budget allocation and per-namespace WAF tracking.
 */

#include "ftl/ns_gc.h"

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/* Find the slot index for a given NSID in the urgency array. */
static int find_urgency_slot(const struct ns_gc_coordinator *coord, u32 nsid) {
    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (coord->urgency[i].active && coord->urgency[i].nsid == nsid) {
            return (int)i;
        }
    }
    return -1;
}

/* Find the first free slot in the urgency array. */
static int find_free_urgency_slot(const struct ns_gc_coordinator *coord) {
    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (!coord->urgency[i].active) {
            return (int)i;
        }
    }
    return -1;
}

/* Clamp a double to [0.0, 1.0]. */
static double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int ns_gc_coordinator_init(struct ns_gc_coordinator *coord, u32 total_budget,
                           double urgency_thresh, double critical_thresh,
                           u32 min_budget) {
    if (!coord) {
        return HFSSS_ERR_INVAL;
    }
    if (total_budget == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(coord, 0, sizeof(*coord));
    coord->total_gc_budget = total_budget;
    coord->urgency_threshold = urgency_thresh;
    coord->critical_threshold = critical_thresh;
    coord->min_budget_per_ns = min_budget;

    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        coord->urgency[i].active = false;
        coord->waf[i].active = false;
    }

    coord->initialized = true;
    return HFSSS_OK;
}

void ns_gc_coordinator_cleanup(struct ns_gc_coordinator *coord) {
    if (!coord || !coord->initialized) {
        return;
    }

    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        coord->urgency[i].active = false;
        coord->waf[i].active = false;
    }

    coord->initialized = false;
}

int ns_gc_register_ns(struct ns_gc_coordinator *coord, u32 nsid,
                      u64 total_blocks) {
    if (!coord || !coord->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0 || total_blocks == 0) {
        return HFSSS_ERR_INVAL;
    }

    /* Check for duplicate */
    if (find_urgency_slot(coord, nsid) >= 0) {
        return HFSSS_ERR_EXIST;
    }

    int slot = find_free_urgency_slot(coord);
    if (slot < 0) {
        return HFSSS_ERR_NOSPC;
    }

    struct ns_gc_urgency *u = &coord->urgency[slot];
    u->nsid = nsid;
    u->score = 0.0;
    u->free_blocks = total_blocks;
    u->total_blocks = total_blocks;
    u->valid_pages = 0;
    u->invalid_pages = 0;
    u->active = true;

    struct ns_waf_tracker *w = &coord->waf[slot];
    w->nsid = nsid;
    w->host_write_pages = 0;
    w->nand_write_pages = 0;
    w->active = true;

    return HFSSS_OK;
}

int ns_gc_unregister_ns(struct ns_gc_coordinator *coord, u32 nsid) {
    if (!coord || !coord->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    int slot = find_urgency_slot(coord, nsid);
    if (slot < 0) {
        return HFSSS_ERR_NOENT;
    }

    memset(&coord->urgency[slot], 0, sizeof(coord->urgency[slot]));
    coord->urgency[slot].active = false;

    memset(&coord->waf[slot], 0, sizeof(coord->waf[slot]));
    coord->waf[slot].active = false;

    return HFSSS_OK;
}

double ns_gc_calculate_urgency(u64 free_blocks, u64 total_blocks,
                               u64 invalid_pages, u64 valid_pages) {
    if (total_blocks == 0) {
        return 0.0;
    }

    double free_ratio = (double)free_blocks / (double)total_blocks;
    double total_pages = (double)(invalid_pages + valid_pages);
    double invalid_ratio = 0.0;
    if (total_pages > 0.0) {
        invalid_ratio = (double)invalid_pages / total_pages;
    }

    double score = (1.0 - free_ratio) * 0.6 + invalid_ratio * 0.4;
    return clamp01(score);
}

int ns_gc_update_urgency(struct ns_gc_coordinator *coord, u32 nsid,
                         u64 free_blocks, u64 total_blocks,
                         u64 invalid_pages, u64 valid_pages) {
    if (!coord || !coord->initialized) {
        return HFSSS_ERR_INVAL;
    }
    if (nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    int slot = find_urgency_slot(coord, nsid);
    if (slot < 0) {
        return HFSSS_ERR_NOENT;
    }

    struct ns_gc_urgency *u = &coord->urgency[slot];
    u->free_blocks = free_blocks;
    u->total_blocks = total_blocks;
    u->valid_pages = valid_pages;
    u->invalid_pages = invalid_pages;
    u->score = ns_gc_calculate_urgency(free_blocks, total_blocks,
                                       invalid_pages, valid_pages);
    return HFSSS_OK;
}

int ns_gc_allocate_budget(struct ns_gc_coordinator *coord, u32 *budgets_out) {
    if (!coord || !coord->initialized || !budgets_out) {
        return HFSSS_ERR_INVAL;
    }

    /* Zero output */
    memset(budgets_out, 0, sizeof(u32) * NS_GC_MAX_NS);

    /* Count active namespaces above urgency threshold and sum scores */
    u32 active_count = 0;
    double total_score = 0.0;
    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (coord->urgency[i].active &&
            coord->urgency[i].score >= coord->urgency_threshold) {
            active_count++;
            total_score += coord->urgency[i].score;
        }
    }

    if (active_count == 0) {
        return HFSSS_OK;
    }

    /* Allocate proportionally with minimum guarantee */
    u32 remaining_budget = coord->total_gc_budget;

    /* First pass: assign minimum budget to each qualifying NS */
    u32 min_total = active_count * coord->min_budget_per_ns;
    if (min_total > coord->total_gc_budget) {
        /* Not enough budget for minimums; distribute evenly */
        u32 per_ns = coord->total_gc_budget / active_count;
        u32 leftover = coord->total_gc_budget % active_count;
        u32 assigned = 0;
        for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
            if (coord->urgency[i].active &&
                coord->urgency[i].score >= coord->urgency_threshold) {
                budgets_out[i] = per_ns + (assigned < leftover ? 1 : 0);
                assigned++;
            }
        }
        return HFSSS_OK;
    }

    remaining_budget -= min_total;

    /* Second pass: distribute remaining budget proportionally */
    u32 distributed = 0;
    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (coord->urgency[i].active &&
            coord->urgency[i].score >= coord->urgency_threshold) {
            budgets_out[i] = coord->min_budget_per_ns;
            if (total_score > 0.0) {
                double proportion = coord->urgency[i].score / total_score;
                u32 extra = (u32)(proportion * (double)remaining_budget);
                budgets_out[i] += extra;
                distributed += extra;
            }
        }
    }

    /* Assign any rounding remainder to the highest-urgency NS */
    u32 remainder = remaining_budget - distributed;
    if (remainder > 0) {
        int best = -1;
        double best_score = -1.0;
        for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
            if (coord->urgency[i].active &&
                coord->urgency[i].score >= coord->urgency_threshold &&
                coord->urgency[i].score > best_score) {
                best_score = coord->urgency[i].score;
                best = (int)i;
            }
        }
        if (best >= 0) {
            budgets_out[best] += remainder;
        }
    }

    return HFSSS_OK;
}

void ns_gc_record_write(struct ns_gc_coordinator *coord, u32 nsid,
                        u64 host_pages, u64 nand_pages) {
    if (!coord || !coord->initialized || nsid == 0) {
        return;
    }

    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (coord->waf[i].active && coord->waf[i].nsid == nsid) {
            coord->waf[i].host_write_pages += host_pages;
            coord->waf[i].nand_write_pages += nand_pages;
            return;
        }
    }
}

double ns_gc_get_waf(const struct ns_gc_coordinator *coord, u32 nsid) {
    if (!coord || !coord->initialized || nsid == 0) {
        return 1.0;
    }

    for (u32 i = 0; i < NS_GC_MAX_NS; i++) {
        if (coord->waf[i].active && coord->waf[i].nsid == nsid) {
            if (coord->waf[i].host_write_pages == 0) {
                return 1.0;
            }
            return (double)coord->waf[i].nand_write_pages /
                   (double)coord->waf[i].host_write_pages;
        }
    }

    return 1.0;
}
