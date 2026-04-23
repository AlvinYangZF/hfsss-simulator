#include "fault_inject.h"
#include "common.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>

#define MODULE "fault_inject"

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

int fault_registry_init(struct fault_registry *reg)
{
    if (!reg)
        return HFSSS_ERR_INVAL;

    memset(reg, 0, sizeof(*reg));

    int rc = mutex_init(&reg->lock);
    if (rc != HFSSS_OK) {
        return rc;
    }

    reg->next_id     = 1;
    reg->initialized = true;
    return HFSSS_OK;
}

void fault_registry_cleanup(struct fault_registry *reg)
{
    if (!reg)
        return;
    if (reg->initialized) {
        mutex_cleanup(&reg->lock);
    }
    memset(reg, 0, sizeof(*reg));
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Rebuild type_present from active entries. Caller holds reg->lock. */
static void rebuild_type_present(struct fault_registry *reg)
{
    uint32_t mask = 0;
    for (int i = 0; i < FAULT_REGISTRY_MAX; i++) {
        if (reg->entries[i].active)
            mask |= (uint32_t)reg->entries[i].type;
    }
    atomic_store_explicit(&reg->type_present, mask, memory_order_release);
}

/* Find an active entry by id; returns index or -1. */
static int find_entry(const struct fault_registry *reg, uint32_t id)
{
    for (int i = 0; i < FAULT_REGISTRY_MAX; i++) {
        if (reg->entries[i].active && reg->entries[i].id == id)
            return i;
    }
    return -1;
}

/* Check whether addr matches fault_addr (FAULT_WILDCARD matches anything). */
static bool addr_matches(const struct fault_addr *pattern,
                         const struct fault_addr *addr)
{
    if (pattern->channel != FAULT_WILDCARD && pattern->channel != addr->channel)
        return false;
    if (pattern->chip    != FAULT_WILDCARD && pattern->chip    != addr->chip)
        return false;
    if (pattern->die     != FAULT_WILDCARD && pattern->die     != addr->die)
        return false;
    if (pattern->plane   != FAULT_WILDCARD && pattern->plane   != addr->plane)
        return false;
    if (pattern->block   != FAULT_WILDCARD && pattern->block   != addr->block)
        return false;
    if (pattern->page    != FAULT_WILDCARD && pattern->page    != addr->page)
        return false;
    return true;
}

/* -------------------------------------------------------------------------
 * Fault management
 * ---------------------------------------------------------------------- */

int fault_inject_add(struct fault_registry *reg,
                     enum fault_type type,
                     const struct fault_addr *addr,
                     enum fault_persist persist,
                     double probability)
{
    if (!reg || !reg->initialized)
        return HFSSS_ERR_INVAL;

    mutex_lock(&reg->lock, 0);

    if (reg->count >= FAULT_REGISTRY_MAX) {
        mutex_unlock(&reg->lock);
        HFSSS_LOG_WARN(MODULE, "Registry full (%d entries)", FAULT_REGISTRY_MAX);
        return HFSSS_ERR;
    }

    /* Find an empty slot. */
    int slot = -1;
    for (int i = 0; i < FAULT_REGISTRY_MAX; i++) {
        if (!reg->entries[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        mutex_unlock(&reg->lock);
        return HFSSS_ERR;
    }

    struct fault_entry *e = &reg->entries[slot];
    memset(e, 0, sizeof(*e));
    e->id          = reg->next_id++;
    e->type        = type;
    e->persist     = persist;
    e->probability = probability;
    e->active      = true;

    if (addr)
        e->addr = *addr;
    else {
        /* Default: wildcard everything. */
        e->addr.channel = FAULT_WILDCARD;
        e->addr.chip    = FAULT_WILDCARD;
        e->addr.die     = FAULT_WILDCARD;
        e->addr.plane   = FAULT_WILDCARD;
        e->addr.block   = FAULT_WILDCARD;
        e->addr.page    = FAULT_WILDCARD;
    }

    reg->count++;
    atomic_fetch_or_explicit(&reg->type_present, (uint32_t)type,
                             memory_order_release);
    uint32_t id = e->id;

    mutex_unlock(&reg->lock);

    HFSSS_LOG_INFO(MODULE, "Added fault id=%u type=0x%x persist=%d prob=%.3f",
                   id, (unsigned)type, (int)persist, probability);
    return (int)id;
}

int fault_inject_set_bit_flip(struct fault_registry *reg,
                               uint32_t id, uint64_t mask)
{
    if (!reg || !reg->initialized)
        return HFSSS_ERR_INVAL;

    mutex_lock(&reg->lock, 0);
    int idx = find_entry(reg, id);
    if (idx < 0) {
        mutex_unlock(&reg->lock);
        return HFSSS_ERR_NOENT;
    }
    reg->entries[idx].bit_flip_mask = mask;
    mutex_unlock(&reg->lock);
    return HFSSS_OK;
}

int fault_inject_set_disturb(struct fault_registry *reg,
                              uint32_t id, double factor)
{
    if (!reg || !reg->initialized)
        return HFSSS_ERR_INVAL;

    mutex_lock(&reg->lock, 0);
    int idx = find_entry(reg, id);
    if (idx < 0) {
        mutex_unlock(&reg->lock);
        return HFSSS_ERR_NOENT;
    }
    reg->entries[idx].disturb_factor = factor;
    mutex_unlock(&reg->lock);
    return HFSSS_OK;
}

int fault_inject_set_aging(struct fault_registry *reg,
                            uint32_t id, double factor)
{
    if (!reg || !reg->initialized)
        return HFSSS_ERR_INVAL;

    mutex_lock(&reg->lock, 0);
    int idx = find_entry(reg, id);
    if (idx < 0) {
        mutex_unlock(&reg->lock);
        return HFSSS_ERR_NOENT;
    }
    reg->entries[idx].aging_factor = factor;
    mutex_unlock(&reg->lock);
    return HFSSS_OK;
}

int fault_inject_remove(struct fault_registry *reg, uint32_t id)
{
    if (!reg || !reg->initialized)
        return HFSSS_ERR_INVAL;

    mutex_lock(&reg->lock, 0);
    int idx = find_entry(reg, id);
    if (idx < 0) {
        mutex_unlock(&reg->lock);
        return HFSSS_ERR_NOENT;
    }
    reg->entries[idx].active = false;
    reg->count--;
    rebuild_type_present(reg);
    mutex_unlock(&reg->lock);
    HFSSS_LOG_INFO(MODULE, "Removed fault id=%u", id);
    return HFSSS_OK;
}

void fault_inject_clear_all(struct fault_registry *reg)
{
    if (!reg)
        return;

    mutex_lock(&reg->lock, 0);
    for (int i = 0; i < FAULT_REGISTRY_MAX; i++)
        reg->entries[i].active = false;

    reg->count        = 0;
    atomic_store_explicit(&reg->type_present, 0u, memory_order_release);
    mutex_unlock(&reg->lock);

    HFSSS_LOG_INFO(MODULE, "Cleared all faults");
}

/* -------------------------------------------------------------------------
 * Hot-path check
 * ---------------------------------------------------------------------- */

struct fault_entry *fault_check(struct fault_registry *reg,
                                 enum fault_type type,
                                 const struct fault_addr *addr)
{
    if (!reg || !reg->initialized || !addr)
        return NULL;

    /*
     * Unlocked fast exit. type_present is atomic; an acquire load
     * pairs with the release store in add / remove / clear_all
     * (performed under reg->lock before unlock). A set-but-stale bit
     * is harmless — we fall into the locked scan and find no match.
     * A clear-but-stale bit means the corresponding add has not yet
     * released its bit to this reader; the next call observes it.
     */
    uint32_t present = atomic_load_explicit(&reg->type_present,
                                            memory_order_acquire);
    if ((present & (uint32_t)type) == 0)
        return NULL;

    struct fault_entry *hit = NULL;

    mutex_lock(&reg->lock, 0);
    for (int i = 0; i < FAULT_REGISTRY_MAX; i++) {
        struct fault_entry *e = &reg->entries[i];

        if (!e->active)
            continue;
        if (e->type != type)
            continue;
        if (!addr_matches(&e->addr, addr))
            continue;

        /* Probability check. */
        if (e->probability < 1.0) {
            double r = (double)rand() / (double)RAND_MAX;
            if (r >= e->probability)
                continue;
        }

        e->hit_count++;

        if (e->persist == FAULT_PERSIST_ONE_SHOT) {
            e->active = false;
            reg->count--;
            rebuild_type_present(reg);
        }

        hit = e;
        break;
    }
    mutex_unlock(&reg->lock);
    return hit;
}

/* -------------------------------------------------------------------------
 * Bit flip
 * ---------------------------------------------------------------------- */

void fault_apply_bit_flip(void *buf, size_t len, uint64_t mask)
{
    if (!buf || len == 0)
        return;

    uint64_t *p   = (uint64_t *)buf;
    size_t    words = len / sizeof(uint64_t);

    for (size_t i = 0; i < words; i++)
        p[i] ^= mask;

    /* Handle trailing bytes. */
    size_t rem = len % sizeof(uint64_t);
    if (rem) {
        uint8_t *tail = (uint8_t *)(p + words);
        uint8_t  mask_bytes[8];
        memcpy(mask_bytes, &mask, sizeof(mask_bytes));
        for (size_t j = 0; j < rem; j++)
            tail[j] ^= mask_bytes[j];
    }
}

/* -------------------------------------------------------------------------
 * Power fault (REQ-133)
 * ---------------------------------------------------------------------- */

int fault_power_write_marker_only(const char *crash_marker_path)
{
    if (!crash_marker_path)
        return HFSSS_ERR_INVAL;

    FILE *f = fopen(crash_marker_path, "w");
    if (!f) {
        HFSSS_LOG_ERROR(MODULE, "Cannot open crash marker path: %s",
                        crash_marker_path);
        return HFSSS_ERR_IO;
    }
    fputs("CRASH\n", f);
    fflush(f);
    fclose(f);
    return HFSSS_OK;
}

void fault_power_inject(const char *crash_marker_path)
{
    fault_power_write_marker_only(crash_marker_path);
    HFSSS_LOG_ERROR(MODULE, "Power fault injected — calling _exit(1)");
    _exit(1);
}

/* -------------------------------------------------------------------------
 * Controller fault (REQ-134)
 * ---------------------------------------------------------------------- */

void fault_controller_panic(const char *reason)
{
    const char *msg = reason ? reason : "(no reason)";
    HFSSS_LOG_ERROR(MODULE, "Controller panic: %s", msg);
    abort();
}

/* -------------------------------------------------------------------------
 * JSON serialization
 * ---------------------------------------------------------------------- */

static const char *fault_type_name(enum fault_type t)
{
    switch (t) {
    case FAULT_NONE:          return "NONE";
    case FAULT_BAD_BLOCK:     return "BAD_BLOCK";
    case FAULT_READ_ERROR:    return "READ_ERROR";
    case FAULT_PROGRAM_ERROR: return "PROGRAM_ERROR";
    case FAULT_ERASE_ERROR:   return "ERASE_ERROR";
    case FAULT_BIT_FLIP:      return "BIT_FLIP";
    case FAULT_READ_DISTURB:  return "READ_DISTURB";
    case FAULT_RETENTION:     return "RETENTION";
    case FAULT_POWER:         return "POWER";
    case FAULT_PANIC:         return "PANIC";
    case FAULT_POOL_EXHAUST:  return "POOL_EXHAUST";
    case FAULT_TIMEOUT:       return "TIMEOUT";
    default:                  return "UNKNOWN";
    }
}

static int append(char *buf, size_t bufsz, size_t *pos, const char *fmt, ...)
{
    if (*pos >= bufsz)
        return HFSSS_ERR_NOSPC;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, bufsz - *pos, fmt, ap);
    va_end(ap);

    if (n < 0)
        return HFSSS_ERR;
    if ((size_t)n >= bufsz - *pos)
        return HFSSS_ERR_NOSPC;

    *pos += (size_t)n;
    return HFSSS_OK;
}

int fault_registry_to_json(const struct fault_registry *reg,
                            char *buf, size_t bufsz)
{
    if (!reg || !buf || bufsz == 0)
        return HFSSS_ERR_INVAL;

    size_t pos = 0;
    int    rc;
    bool   first = true;

    rc = append(buf, bufsz, &pos, "[");
    if (rc != HFSSS_OK)
        return rc;

    /*
     * Walk entries under reg->lock so the snapshot sees a consistent
     * view of active / hit_count / persist against concurrent
     * fault_inject_add / remove / clear_all / fault_check mutations.
     * const is cast away only to acquire the lock — the entries
     * themselves are read, not written.
     */
    struct fault_registry *reg_mut = (struct fault_registry *)reg;
    if (reg_mut->initialized)
        mutex_lock(&reg_mut->lock, 0);

    for (int i = 0; i < FAULT_REGISTRY_MAX; i++) {
        const struct fault_entry *e = &reg->entries[i];
        if (!e->active && e->id == 0)
            continue;
        /* Include all slots that have been used (id != 0). */
        if (e->id == 0)
            continue;

        if (!first) {
            rc = append(buf, bufsz, &pos, ",");
            if (rc != HFSSS_OK) {
                if (reg_mut->initialized)
                    mutex_unlock(&reg_mut->lock);
                return rc;
            }
        }
        first = false;

        rc = append(buf, bufsz, &pos,
            "{\"id\":%u,\"type\":\"%s\","
            "\"addr\":{\"channel\":%u,\"chip\":%u,\"die\":%u,"
            "\"plane\":%u,\"block\":%u,\"page\":%u},"
            "\"persist\":%d,"
            "\"hit_count\":%" PRIu64 ","
            "\"active\":%s}",
            e->id,
            fault_type_name(e->type),
            e->addr.channel, e->addr.chip, e->addr.die,
            e->addr.plane, e->addr.block, e->addr.page,
            (int)e->persist,
            e->hit_count,
            e->active ? "true" : "false");
        if (rc != HFSSS_OK) {
            if (reg_mut->initialized)
                mutex_unlock(&reg_mut->lock);
            return rc;
        }
    }

    if (reg_mut->initialized)
        mutex_unlock(&reg_mut->lock);

    rc = append(buf, bufsz, &pos, "]");
    return rc;
}

int fault_entry_from_json(const char *json, struct fault_entry *out)
{
    if (!json || !out)
        return HFSSS_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    /* Minimal parser: extract key numeric/string fields. */
    unsigned id = 0, persist = 0;
    uint32_t ch = FAULT_WILDCARD, chip = FAULT_WILDCARD,
             die = FAULT_WILDCARD, plane = FAULT_WILDCARD,
             blk = FAULT_WILDCARD, pg = FAULT_WILDCARD;
    char     type_str[32] = {0};
    uint64_t hit = 0;

    /* id */
    const char *p = strstr(json, "\"id\":");
    if (p) sscanf(p + 5, "%u", &id);

    /* type */
    p = strstr(json, "\"type\":");
    if (p) {
        p += 7;
        while (*p == ' ' || *p == '"') p++;
        int j = 0;
        while (*p && *p != '"' && j < 31)
            type_str[j++] = *p++;
    }

    /* addr fields */
    p = strstr(json, "\"channel\":");
    if (p) sscanf(p + 10, "%u", &ch);
    p = strstr(json, "\"chip\":");
    if (p) sscanf(p + 7, "%u", &chip);
    p = strstr(json, "\"die\":");
    if (p) sscanf(p + 6, "%u", &die);
    p = strstr(json, "\"plane\":");
    if (p) sscanf(p + 8, "%u", &plane);
    p = strstr(json, "\"block\":");
    if (p) sscanf(p + 8, "%u", &blk);
    p = strstr(json, "\"page\":");
    if (p) sscanf(p + 7, "%u", &pg);

    /* persist */
    p = strstr(json, "\"persist\":");
    if (p) sscanf(p + 10, "%u", &persist);

    /* hit_count */
    p = strstr(json, "\"hit_count\":");
    if (p) sscanf(p + 12, "%" SCNu64, &hit);

    out->id             = id;
    out->addr.channel   = ch;
    out->addr.chip      = chip;
    out->addr.die       = die;
    out->addr.plane     = plane;
    out->addr.block     = blk;
    out->addr.page      = pg;
    out->persist        = (enum fault_persist)persist;
    out->hit_count      = hit;
    out->active         = true;

    /* Map type string to enum. */
    if      (strcmp(type_str, "BAD_BLOCK")     == 0) out->type = FAULT_BAD_BLOCK;
    else if (strcmp(type_str, "READ_ERROR")    == 0) out->type = FAULT_READ_ERROR;
    else if (strcmp(type_str, "PROGRAM_ERROR") == 0) out->type = FAULT_PROGRAM_ERROR;
    else if (strcmp(type_str, "ERASE_ERROR")   == 0) out->type = FAULT_ERASE_ERROR;
    else if (strcmp(type_str, "BIT_FLIP")      == 0) out->type = FAULT_BIT_FLIP;
    else if (strcmp(type_str, "READ_DISTURB")  == 0) out->type = FAULT_READ_DISTURB;
    else if (strcmp(type_str, "RETENTION")     == 0) out->type = FAULT_RETENTION;
    else if (strcmp(type_str, "POWER")         == 0) out->type = FAULT_POWER;
    else if (strcmp(type_str, "PANIC")         == 0) out->type = FAULT_PANIC;
    else if (strcmp(type_str, "POOL_EXHAUST")  == 0) out->type = FAULT_POOL_EXHAUST;
    else if (strcmp(type_str, "TIMEOUT")       == 0) out->type = FAULT_TIMEOUT;
    else                                              out->type = FAULT_NONE;

    return HFSSS_OK;
}
