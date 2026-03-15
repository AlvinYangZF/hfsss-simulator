#ifndef HFSSS_FAULT_INJECT_H
#define HFSSS_FAULT_INJECT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fault types */
enum fault_type {
    FAULT_NONE          = 0,
    FAULT_BAD_BLOCK     = (1 << 0),
    FAULT_READ_ERROR    = (1 << 1),
    FAULT_PROGRAM_ERROR = (1 << 2),
    FAULT_ERASE_ERROR   = (1 << 3),
    FAULT_BIT_FLIP      = (1 << 4),
    FAULT_READ_DISTURB  = (1 << 5),
    FAULT_RETENTION     = (1 << 6),
    FAULT_POWER         = (1 << 7),
    FAULT_PANIC         = (1 << 8),
    FAULT_POOL_EXHAUST  = (1 << 9),
};

/* Persistence mode */
enum fault_persist {
    FAULT_PERSIST_ONE_SHOT = 0,  /* auto-clears after first hit */
    FAULT_PERSIST_STICKY   = 1,  /* remains until cleared */
};

#define FAULT_WILDCARD  UINT32_MAX  /* match any value in that field */

/* Fault target address (NAND hierarchy) */
struct fault_addr {
    uint32_t channel;   /* FAULT_WILDCARD = any */
    uint32_t chip;
    uint32_t die;
    uint32_t plane;
    uint32_t block;
    uint32_t page;
};

/* Fault descriptor */
struct fault_entry {
    uint32_t           id;           /* auto-assigned */
    enum fault_type    type;
    struct fault_addr  addr;
    enum fault_persist persist;
    uint64_t           bit_flip_mask;  /* XOR mask for FAULT_BIT_FLIP */
    double             probability;    /* 0.0-1.0; 1.0 = always */
    double             disturb_factor; /* read disturb amplification */
    double             aging_factor;   /* retention aging multiplier */
    uint64_t           hit_count;      /* incremented each trigger */
    bool               active;
};

#define FAULT_REGISTRY_MAX  256

/* Fault registry */
struct fault_registry {
    struct fault_entry entries[FAULT_REGISTRY_MAX];
    int                count;
    uint32_t           next_id;
    uint32_t           type_present;  /* bitmask: lock-free fast exit */
    bool               initialized;
};

/* Lifecycle */
int  fault_registry_init(struct fault_registry *reg);
void fault_registry_cleanup(struct fault_registry *reg);

/* Add a fault; returns assigned id or -1 on error */
int  fault_inject_add(struct fault_registry *reg,
                      enum fault_type type,
                      const struct fault_addr *addr,
                      enum fault_persist persist,
                      double probability);

/* Set bit flip mask for an existing fault */
int  fault_inject_set_bit_flip(struct fault_registry *reg,
                                uint32_t id, uint64_t mask);

/* Set read disturb factor */
int  fault_inject_set_disturb(struct fault_registry *reg,
                               uint32_t id, double factor);

/* Set data retention aging factor */
int  fault_inject_set_aging(struct fault_registry *reg,
                             uint32_t id, double factor);

/* Remove a fault by id */
int  fault_inject_remove(struct fault_registry *reg, uint32_t id);

/* Clear all faults */
void fault_inject_clear_all(struct fault_registry *reg);

/*
 * Check whether a NAND operation should fault.
 *
 * Returns the matching entry (and increments hit_count), or NULL.
 * Fast exit via type_present bitmask when no fault of that type is registered.
 * One-shot faults are deactivated after the first hit.
 */
struct fault_entry *fault_check(struct fault_registry *reg,
                                 enum fault_type type,
                                 const struct fault_addr *addr);

/* Apply bit flip to buffer (XOR each 8-byte word with mask) */
void fault_apply_bit_flip(void *buf, size_t len, uint64_t mask);

/* Power fault: write crash marker then _exit(1) -- REQ-133 */
void fault_power_inject(const char *crash_marker_path);

/* Write crash marker without exiting (for testing) */
int  fault_power_write_marker_only(const char *crash_marker_path);

/* Controller fault: trigger Panic -- REQ-134 */
void fault_controller_panic(const char *reason);

/* JSON serialization for OOB fault.list */
int fault_registry_to_json(const struct fault_registry *reg,
                            char *buf, size_t bufsz);

/* JSON deserialization for OOB fault.inject */
int fault_entry_from_json(const char *json, struct fault_entry *out);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_FAULT_INJECT_H */
