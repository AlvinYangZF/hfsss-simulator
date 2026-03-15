#ifndef HFSSS_COMMON_CONFIG_H
#define HFSSS_COMMON_CONFIG_H

/*
 * HFSSS YAML-style configuration file parser (REQ-130)
 *
 * Supports a subset of YAML: key: value pairs, sections (key:), comments (#).
 * No arrays, no multi-line strings.  Values may be quoted or unquoted.
 * Unknown keys produce a warning, not a fatal error.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HFSSS_CFG_STR_LEN   256
#define HFSSS_CFG_PATH_LEN  512
#define HFSSS_CFG_MAX_KEYS  128

/* NAND geometry configuration */
struct hfsss_nand_cfg {
    uint32_t channel_count;
    uint32_t chips_per_channel;
    uint32_t dies_per_chip;
    uint32_t planes_per_die;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;
    uint32_t page_size;
    uint32_t spare_size;
    uint32_t op_ratio_pct;       /* over-provisioning % */
};

/* GC configuration */
struct hfsss_gc_cfg {
    uint32_t threshold_pct;      /* trigger GC when free < threshold% */
    uint32_t hiwater_pct;
    uint32_t lowater_pct;
    char     policy[32];         /* "greedy" or "cost_benefit" */
};

/* OOB server configuration */
struct hfsss_oob_cfg {
    char     sock_path[HFSSS_CFG_PATH_LEN];
    uint32_t max_clients;
    bool     enabled;
};

/* Performance limits */
struct hfsss_perf_cfg {
    uint32_t temp_warn_celsius;
    uint32_t temp_crit_celsius;
    uint32_t spare_warn_pct;
    uint32_t latency_warn_us;
};

/* Persistence configuration */
struct hfsss_persist_cfg {
    char     nand_image_path[HFSSS_CFG_PATH_LEN];
    char     nor_image_path[HFSSS_CFG_PATH_LEN];
    char     wal_path[HFSSS_CFG_PATH_LEN];
    char     checkpoint_path[HFSSS_CFG_PATH_LEN];
    uint32_t checkpoint_interval_s;
};

/* Top-level simulator configuration */
struct hfsss_config {
    struct hfsss_nand_cfg    nand;
    struct hfsss_gc_cfg      gc;
    struct hfsss_oob_cfg     oob;
    struct hfsss_perf_cfg    perf;
    struct hfsss_persist_cfg persist;

    uint32_t log_level;          /* 0=ERROR 1=WARN 2=INFO 3=DEBUG 4=TRACE */
    char     log_file[HFSSS_CFG_PATH_LEN];
    bool     loaded;
};

/* ------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------ */

/* Initialize config with default values */
void hfsss_config_defaults(struct hfsss_config *cfg);

/* Load from YAML-style file.  Unknown keys produce a warning.
 * Returns 0 on success, negative on I/O or parse error.
 * Error details written to errbuf (may be NULL).               */
int  hfsss_config_load(const char *path, struct hfsss_config *cfg,
                       char *errbuf, size_t errbuf_len);

/* Save current config back to file in YAML format */
int  hfsss_config_save(const char *path, const struct hfsss_config *cfg);

/* Get/set individual keys by dotted path (e.g. "nand.channel_count").
 * Used by the OOB config.get / config.set JSON-RPC handlers.        */
int  hfsss_config_get_str(const struct hfsss_config *cfg,
                           const char *key, char *out, size_t outlen);
int  hfsss_config_set_str(struct hfsss_config *cfg,
                           const char *key, const char *value);

/* Validate — returns 0 if valid, negative with errbuf filled if not */
int  hfsss_config_validate(const struct hfsss_config *cfg,
                            char *errbuf, size_t errbuf_len);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_COMMON_CONFIG_H */
