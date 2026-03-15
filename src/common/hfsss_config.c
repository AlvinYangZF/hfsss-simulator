#include "common/hfsss_config.h"
#include "common/oob.h"
#include "common/log.h"
#include "common/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------ */
void hfsss_config_defaults(struct hfsss_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    /* NAND: modest test geometry */
    cfg->nand.channel_count     = 8;
    cfg->nand.chips_per_channel = 8;
    cfg->nand.dies_per_chip     = 2;
    cfg->nand.planes_per_die    = 2;
    cfg->nand.blocks_per_plane  = 1024;
    cfg->nand.pages_per_block   = 256;
    cfg->nand.page_size         = 4096;
    cfg->nand.spare_size        = 64;
    cfg->nand.op_ratio_pct      = 7;

    /* GC */
    cfg->gc.threshold_pct = 10;
    cfg->gc.hiwater_pct   = 15;
    cfg->gc.lowater_pct   = 5;
    strncpy(cfg->gc.policy, "greedy", sizeof(cfg->gc.policy) - 1);

    /* OOB */
    strncpy(cfg->oob.sock_path, OOB_DEFAULT_SOCK, HFSSS_CFG_PATH_LEN - 1);
    cfg->oob.max_clients = 16;
    cfg->oob.enabled     = true;

    /* Perf */
    cfg->perf.temp_warn_celsius = TEMP_WARN_CELSIUS;
    cfg->perf.temp_crit_celsius = TEMP_CRIT_CELSIUS;
    cfg->perf.spare_warn_pct    = SMART_DEFAULT_SPARE_THRESH;
    cfg->perf.latency_warn_us   = 5000;

    /* Persistence */
    strncpy(cfg->persist.nand_image_path, "/tmp/hfsss_nand.bin",
            HFSSS_CFG_PATH_LEN - 1);
    strncpy(cfg->persist.nor_image_path,  "/tmp/hfsss_nor.bin",
            HFSSS_CFG_PATH_LEN - 1);
    strncpy(cfg->persist.wal_path,        "/tmp/hfsss_wal.bin",
            HFSSS_CFG_PATH_LEN - 1);
    strncpy(cfg->persist.checkpoint_path, "/tmp/hfsss_checkpoint.bin",
            HFSSS_CFG_PATH_LEN - 1);
    cfg->persist.checkpoint_interval_s = 30;

    cfg->log_level = LOG_LEVEL_INFO;
    cfg->loaded = false;
}

/* ------------------------------------------------------------------
 * Internal: trim leading/trailing whitespace in place
 * ------------------------------------------------------------------ */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

/* Remove surrounding quotes from value (single or double) */
static void unquote(char *s) {
    size_t len = strlen(s);
    if (len >= 2 &&
        ((s[0] == '"' && s[len-1] == '"') ||
         (s[0] == '\'' && s[len-1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

/* ------------------------------------------------------------------
 * Apply a key=value pair to config
 * Section prefixes are tracked by the parser loop.
 * Returns 0 if key recognized, 1 if unknown (caller logs warning).
 * ------------------------------------------------------------------ */
static int apply_kv(struct hfsss_config *cfg,
                    const char *section, const char *key, const char *val) {
    /* Build full dotted key */
    char fkey[128];
    if (section[0])
        snprintf(fkey, sizeof(fkey), "%s.%s", section, key);
    else
        strncpy(fkey, key, sizeof(fkey) - 1);

    /* nand.* */
    if (!strcmp(fkey, "nand.channel_count"))       { cfg->nand.channel_count      = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.chips_per_channel"))   { cfg->nand.chips_per_channel  = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.dies_per_chip"))       { cfg->nand.dies_per_chip      = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.planes_per_die"))      { cfg->nand.planes_per_die     = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.blocks_per_plane"))    { cfg->nand.blocks_per_plane   = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.pages_per_block"))     { cfg->nand.pages_per_block    = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.page_size"))           { cfg->nand.page_size          = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.spare_size"))          { cfg->nand.spare_size         = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "nand.op_ratio_pct"))        { cfg->nand.op_ratio_pct       = (uint32_t)atoi(val); return 0; }

    /* gc.* */
    if (!strcmp(fkey, "gc.threshold_pct")) { cfg->gc.threshold_pct = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "gc.hiwater_pct"))   { cfg->gc.hiwater_pct   = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "gc.lowater_pct"))   { cfg->gc.lowater_pct   = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "gc.policy"))        { strncpy(cfg->gc.policy, val, sizeof(cfg->gc.policy) - 1); return 0; }

    /* oob.* */
    if (!strcmp(fkey, "oob.sock_path"))   { strncpy(cfg->oob.sock_path, val, HFSSS_CFG_PATH_LEN - 1); return 0; }
    if (!strcmp(fkey, "oob.max_clients")) { cfg->oob.max_clients = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "oob.enabled"))     { cfg->oob.enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0); return 0; }

    /* perf.* */
    if (!strcmp(fkey, "perf.temp_warn_celsius")) { cfg->perf.temp_warn_celsius = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "perf.temp_crit_celsius")) { cfg->perf.temp_crit_celsius = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "perf.spare_warn_pct"))    { cfg->perf.spare_warn_pct    = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "perf.latency_warn_us"))   { cfg->perf.latency_warn_us   = (uint32_t)atoi(val); return 0; }

    /* persist.* */
    if (!strcmp(fkey, "persist.nand_image_path"))      { strncpy(cfg->persist.nand_image_path, val, HFSSS_CFG_PATH_LEN - 1); return 0; }
    if (!strcmp(fkey, "persist.nor_image_path"))       { strncpy(cfg->persist.nor_image_path,  val, HFSSS_CFG_PATH_LEN - 1); return 0; }
    if (!strcmp(fkey, "persist.wal_path"))             { strncpy(cfg->persist.wal_path,        val, HFSSS_CFG_PATH_LEN - 1); return 0; }
    if (!strcmp(fkey, "persist.checkpoint_path"))      { strncpy(cfg->persist.checkpoint_path, val, HFSSS_CFG_PATH_LEN - 1); return 0; }
    if (!strcmp(fkey, "persist.checkpoint_interval_s")){ cfg->persist.checkpoint_interval_s = (uint32_t)atoi(val); return 0; }

    /* top-level */
    if (!strcmp(fkey, "log_level")) { cfg->log_level = (uint32_t)atoi(val); return 0; }
    if (!strcmp(fkey, "log_file"))  { strncpy(cfg->log_file, val, HFSSS_CFG_PATH_LEN - 1); return 0; }

    return 1;  /* unknown key */
}

/* ------------------------------------------------------------------
 * Load from file
 * ------------------------------------------------------------------ */
int hfsss_config_load(const char *path, struct hfsss_config *cfg,
                      char *errbuf, size_t errbuf_len) {
    if (!path || !cfg) return HFSSS_ERR_INVAL;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errbuf)
            snprintf(errbuf, errbuf_len, "cannot open %s: %s",
                     path, strerror(errno));
        return HFSSS_ERR_NOENT;
    }

    hfsss_config_defaults(cfg);

    char line[512];
    char section[64] = "";
    int  lineno = 0;
    int  ret = HFSSS_OK;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;

        /* Section header: "section_name:" with no value on the same line */
        char *colon = strchr(p, ':');
        if (!colon) continue;  /* malformed line */

        *colon = '\0';
        char *key = trim(p);
        char *val = trim(colon + 1);

        if (*val == '\0') {
            /* Section header */
            strncpy(section, key, sizeof(section) - 1);
            continue;
        }

        unquote(val);

        if (apply_kv(cfg, section, key, val) != 0) {
            HFSSS_LOG_WARN("CFG", "unknown key '%s.%s' at line %d (ignored)",
                           section, key, lineno);
        }
    }

    fclose(f);
    cfg->loaded = true;

    if (hfsss_config_validate(cfg, errbuf, errbuf_len) != HFSSS_OK)
        ret = HFSSS_ERR_INVAL;

    return ret;
}

/* ------------------------------------------------------------------
 * Save to file
 * ------------------------------------------------------------------ */
int hfsss_config_save(const char *path, const struct hfsss_config *cfg) {
    if (!path || !cfg) return HFSSS_ERR_INVAL;

    FILE *f = fopen(path, "w");
    if (!f) return HFSSS_ERR_IO;

    fprintf(f, "# HFSSS configuration\n\n");

    fprintf(f, "nand:\n");
    fprintf(f, "  channel_count: %u\n",     cfg->nand.channel_count);
    fprintf(f, "  chips_per_channel: %u\n", cfg->nand.chips_per_channel);
    fprintf(f, "  dies_per_chip: %u\n",     cfg->nand.dies_per_chip);
    fprintf(f, "  planes_per_die: %u\n",    cfg->nand.planes_per_die);
    fprintf(f, "  blocks_per_plane: %u\n",  cfg->nand.blocks_per_plane);
    fprintf(f, "  pages_per_block: %u\n",   cfg->nand.pages_per_block);
    fprintf(f, "  page_size: %u\n",         cfg->nand.page_size);
    fprintf(f, "  spare_size: %u\n",        cfg->nand.spare_size);
    fprintf(f, "  op_ratio_pct: %u\n",      cfg->nand.op_ratio_pct);

    fprintf(f, "\ngc:\n");
    fprintf(f, "  threshold_pct: %u\n", cfg->gc.threshold_pct);
    fprintf(f, "  hiwater_pct: %u\n",   cfg->gc.hiwater_pct);
    fprintf(f, "  lowater_pct: %u\n",   cfg->gc.lowater_pct);
    fprintf(f, "  policy: %s\n",        cfg->gc.policy);

    fprintf(f, "\noob:\n");
    fprintf(f, "  sock_path: %s\n",   cfg->oob.sock_path);
    fprintf(f, "  max_clients: %u\n", cfg->oob.max_clients);
    fprintf(f, "  enabled: %s\n",     cfg->oob.enabled ? "true" : "false");

    fprintf(f, "\nperf:\n");
    fprintf(f, "  temp_warn_celsius: %u\n", cfg->perf.temp_warn_celsius);
    fprintf(f, "  temp_crit_celsius: %u\n", cfg->perf.temp_crit_celsius);
    fprintf(f, "  spare_warn_pct: %u\n",    cfg->perf.spare_warn_pct);
    fprintf(f, "  latency_warn_us: %u\n",   cfg->perf.latency_warn_us);

    fprintf(f, "\npersist:\n");
    fprintf(f, "  nand_image_path: %s\n",      cfg->persist.nand_image_path);
    fprintf(f, "  nor_image_path: %s\n",       cfg->persist.nor_image_path);
    fprintf(f, "  wal_path: %s\n",             cfg->persist.wal_path);
    fprintf(f, "  checkpoint_path: %s\n",      cfg->persist.checkpoint_path);
    fprintf(f, "  checkpoint_interval_s: %u\n",cfg->persist.checkpoint_interval_s);

    fprintf(f, "\nlog_level: %u\n", cfg->log_level);
    if (cfg->log_file[0])
        fprintf(f, "log_file: %s\n", cfg->log_file);

    fclose(f);
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Validate
 * ------------------------------------------------------------------ */
int hfsss_config_validate(const struct hfsss_config *cfg,
                           char *errbuf, size_t errbuf_len) {
    if (!cfg) return HFSSS_ERR_INVAL;

    if (cfg->nand.channel_count == 0) {
        if (errbuf) snprintf(errbuf, errbuf_len, "nand.channel_count must be > 0");
        return HFSSS_ERR_INVAL;
    }
    if (cfg->nand.page_size == 0 || (cfg->nand.page_size & (cfg->nand.page_size - 1)) != 0) {
        if (errbuf) snprintf(errbuf, errbuf_len, "nand.page_size must be a power of 2");
        return HFSSS_ERR_INVAL;
    }
    if (cfg->gc.threshold_pct > 100) {
        if (errbuf) snprintf(errbuf, errbuf_len, "gc.threshold_pct must be 0-100");
        return HFSSS_ERR_INVAL;
    }
    if (cfg->perf.temp_warn_celsius >= cfg->perf.temp_crit_celsius) {
        if (errbuf) snprintf(errbuf, errbuf_len,
                             "perf.temp_warn_celsius must be < temp_crit_celsius");
        return HFSSS_ERR_INVAL;
    }
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Get/set by dotted key
 * ------------------------------------------------------------------ */
int hfsss_config_get_str(const struct hfsss_config *cfg,
                          const char *key, char *out, size_t outlen) {
    if (!cfg || !key || !out) return HFSSS_ERR_INVAL;

#define CFG_GET_U32(k, field) \
    if (!strcmp(key, k)) { snprintf(out, outlen, "%u", cfg->field); return HFSSS_OK; }
#define CFG_GET_STR(k, field) \
    if (!strcmp(key, k)) { strncpy(out, cfg->field, outlen - 1); out[outlen-1]='\0'; return HFSSS_OK; }
#define CFG_GET_BOOL(k, field) \
    if (!strcmp(key, k)) { strncpy(out, cfg->field ? "true" : "false", outlen - 1); return HFSSS_OK; }

    CFG_GET_U32("nand.channel_count",      nand.channel_count)
    CFG_GET_U32("nand.page_size",          nand.page_size)
    CFG_GET_U32("gc.threshold_pct",        gc.threshold_pct)
    CFG_GET_STR("gc.policy",               gc.policy)
    CFG_GET_STR("oob.sock_path",           oob.sock_path)
    CFG_GET_U32("oob.max_clients",         oob.max_clients)
    CFG_GET_BOOL("oob.enabled",            oob.enabled)
    CFG_GET_U32("perf.temp_warn_celsius",  perf.temp_warn_celsius)
    CFG_GET_U32("perf.spare_warn_pct",     perf.spare_warn_pct)
    CFG_GET_STR("persist.nand_image_path", persist.nand_image_path)
    CFG_GET_U32("persist.checkpoint_interval_s", persist.checkpoint_interval_s)
    CFG_GET_U32("log_level",               log_level)

    return HFSSS_ERR_NOENT;
}

int hfsss_config_set_str(struct hfsss_config *cfg,
                          const char *key, const char *value) {
    if (!cfg || !key || !value) return HFSSS_ERR_INVAL;

    char section[64] = "";
    char k[128];
    const char *dot = strchr(key, '.');
    if (dot) {
        size_t slen = (size_t)(dot - key);
        if (slen >= sizeof(section)) slen = sizeof(section) - 1;
        strncpy(section, key, slen);
        section[slen] = '\0';
        strncpy(k, dot + 1, sizeof(k) - 1);
    } else {
        strncpy(k, key, sizeof(k) - 1);
    }
    k[sizeof(k) - 1] = '\0';

    return (apply_kv(cfg, section, k, value) == 0) ? HFSSS_OK : HFSSS_ERR_NOENT;
}
