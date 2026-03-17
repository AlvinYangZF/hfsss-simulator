#include "common/boot.h"
#include "common/log.h"
#include "common/common.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>

/* ------------------------------------------------------------------
 * CRC-32 (IEEE 802.3 polynomial)
 * ------------------------------------------------------------------ */
static uint32_t crc32_table[256];
static bool     crc32_table_ready = false;

static void crc32_build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

uint32_t hfsss_crc32(const void *data, size_t len) {
    if (!crc32_table_ready) {
        crc32_build_table();
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------
 * Time helpers
 * ------------------------------------------------------------------ */
static uint64_t get_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t get_realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------
 * Boot log helpers
 * ------------------------------------------------------------------ */
static void boot_log(struct boot_ctx *ctx, uint8_t level, const char *msg) {
    if (!ctx || ctx->log_count >= BOOT_LOG_ENTRY_MAX) {
        return;
    }
    struct boot_log_entry *e = &ctx->log[ctx->log_count++];
    e->timestamp_ns = get_ns() - ctx->boot_start_ns;
    e->phase        = (uint8_t)ctx->current_phase;
    e->level        = level;
    strncpy(e->msg, msg, BOOT_LOG_MSG_LEN - 1);
    e->msg[BOOT_LOG_MSG_LEN - 1] = '\0';

    if (level == 0) {
        HFSSS_LOG_INFO("BOOT", "[Phase %d] %s", ctx->current_phase, msg);
    } else if (level == 1) {
        HFSSS_LOG_WARN("BOOT", "[Phase %d] %s", ctx->current_phase, msg);
    } else {
        HFSSS_LOG_ERROR("BOOT", "[Phase %d] %s", ctx->current_phase, msg);
    }
}

/* ------------------------------------------------------------------
 * SysInfo helpers
 * ------------------------------------------------------------------ */
void boot_sysinfo_init(struct sysinfo_partition *si) {
    memset(si, 0, sizeof(*si));
    si->magic      = SYSINFO_MAGIC;
    si->active_slot = 0xFF;
    si->crc32 = hfsss_crc32(si, sizeof(*si) - sizeof(si->crc32));
}

int boot_sysinfo_verify(const struct sysinfo_partition *si) {
    if (!si) {
        return HFSSS_ERR_INVAL;
    }
    if (si->magic == 0xFFFFFFFFu) {
        return HFSSS_ERR_NOENT;  /* blank/erased */
    }
    if (si->magic != SYSINFO_MAGIC) {
        return HFSSS_ERR_IO;
    }
    uint32_t expected = hfsss_crc32(si, sizeof(*si) - sizeof(si->crc32));
    return (expected == si->crc32) ? HFSSS_OK : HFSSS_ERR_IO;
}

void boot_sysinfo_stamp_boot(struct sysinfo_partition *si, enum boot_type bt) {
    si->boot_count++;
    si->last_boot_ns     = get_realtime_ns();
    si->boot_type_last   = (uint8_t)bt;
    /* Clear crash/clean markers for this new boot */
    si->clean_shutdown_marker_valid = 0;
    si->crash_marker_valid          = 0;
    si->clean_shutdown_marker       = 0;
    si->crash_marker                = 0;
    si->crc32 = hfsss_crc32(si, sizeof(*si) - sizeof(si->crc32));
}

/* ------------------------------------------------------------------
 * Firmware slot helpers (REQ-079)
 * ------------------------------------------------------------------ */
bool boot_slot_valid(const struct nor_firmware_slot *slot) {
    if (!slot) return false;
    return (slot->magic == NOR_SLOT_MAGIC &&
            slot->version != NOR_SLOT_VERSION_INVALID);
}

int boot_select_firmware_slot(struct boot_ctx *ctx) {
    bool a_valid = boot_slot_valid(&ctx->slots[0]);
    bool b_valid = boot_slot_valid(&ctx->slots[1]);

    if (!a_valid && !b_valid) {
        boot_log(ctx, 2, "No valid firmware slot — recovery mode");
        return HFSSS_ERR_NOENT;
    }
    if (a_valid && b_valid) {
        ctx->active_slot = (ctx->slots[0].version >= ctx->slots[1].version) ? 0 : 1;
    } else {
        ctx->active_slot = a_valid ? 0 : 1;
    }
    ctx->sysinfo.active_slot = ctx->active_slot;

    char msg[BOOT_LOG_MSG_LEN];
    snprintf(msg, sizeof(msg), "Selected firmware Slot %c version=%u",
             ctx->active_slot == 0 ? 'A' : 'B',
             ctx->slots[ctx->active_slot].version);
    boot_log(ctx, 0, msg);
    return HFSSS_OK;
}

int boot_swap_firmware_slot(struct boot_ctx *ctx) {
    if (!ctx) return HFSSS_ERR_INVAL;
    uint8_t new_slot = (ctx->active_slot == 0) ? 1 : 0;
    if (!boot_slot_valid(&ctx->slots[new_slot])) {
        return HFSSS_ERR_NOENT;
    }
    ctx->slots[ctx->active_slot].active = 0;
    ctx->slots[new_slot].active         = 1;
    ctx->active_slot                    = new_slot;
    ctx->sysinfo.active_slot            = new_slot;
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Determine boot type from SysInfo (REQ-080)
 * ------------------------------------------------------------------ */
static enum boot_type detect_boot_type(const struct sysinfo_partition *si) {
    if (boot_sysinfo_verify(si) != HFSSS_OK) {
        return BOOT_FIRST;
    }
    if (si->crash_marker_valid && si->crash_marker == SYSINFO_CRASH_MARKER) {
        return BOOT_RECOVERY;
    }
    if (si->clean_shutdown_marker_valid &&
        si->clean_shutdown_marker == SYSINFO_CLEAN_MARKER) {
        return BOOT_NORMAL;
    }
    return BOOT_RECOVERY;
}

/* ------------------------------------------------------------------
 * Individual boot phases
 * ------------------------------------------------------------------ */
static int phase0_hw_init(struct boot_ctx *ctx) {
    boot_log(ctx, 0, "HW Init: NAND/NOR power-on, memory pool init");
    /* Simulate 10ms hardware init */
    struct timespec delay = { 0, 10 * 1000000L };
    nanosleep(&delay, NULL);
    return HFSSS_OK;
}

static int phase1_post(struct boot_ctx *ctx) {
    boot_log(ctx, 0, "POST: scanning NOR slots, verifying CRC");
    int ret = boot_select_firmware_slot(ctx);
    if (ret != HFSSS_OK) {
        boot_log(ctx, 1, "POST: no valid slot, continuing with defaults");
    }
    struct timespec delay = { 0, 20 * 1000000L };
    nanosleep(&delay, NULL);
    return HFSSS_OK;
}

static int phase2_meta_load(struct boot_ctx *ctx) {
    ctx->boot_type = detect_boot_type(&ctx->sysinfo);

    char msg[BOOT_LOG_MSG_LEN];
    static const char *type_names[] = {
        "FIRST", "NORMAL", "RECOVERY", "DEGRADED"
    };
    snprintf(msg, sizeof(msg), "Meta Load: boot_type=%s",
             type_names[ctx->boot_type]);
    boot_log(ctx, 0, msg);

    switch (ctx->boot_type) {
    case BOOT_FIRST:
        boot_log(ctx, 0, "Meta Load: first boot, formatting metadata");
        break;
    case BOOT_NORMAL:
        boot_log(ctx, 0, "Meta Load: loading L2P checkpoint");
        break;
    case BOOT_RECOVERY:
        boot_log(ctx, 0, "Meta Load: replaying WAL after abnormal shutdown");
        break;
    default:
        break;
    }
    /* Simulate 50ms metadata load */
    struct timespec delay = { 0, 50 * 1000000L };
    nanosleep(&delay, NULL);
    return HFSSS_OK;
}

static int phase3_ctrl_init(struct boot_ctx *ctx) {
    boot_log(ctx, 0, "Ctrl Init: arbiter, write buffer, read cache, LB");
    struct timespec delay = { 0, 20 * 1000000L };
    nanosleep(&delay, NULL);
    return HFSSS_OK;
}

static int phase4_nvme_init(struct boot_ctx *ctx) {
    boot_log(ctx, 0, "NVMe Init: queue structures, CSTS.RDY=1");
    struct timespec delay = { 0, 10 * 1000000L };
    nanosleep(&delay, NULL);
    return HFSSS_OK;
}

static int phase5_ready(struct boot_ctx *ctx) {
    boot_log(ctx, 0, "Ready: OOB listener, monitoring active, I/O open");
    return HFSSS_OK;
}

/* ------------------------------------------------------------------
 * Public boot API
 * ------------------------------------------------------------------ */
int boot_ctx_init(struct boot_ctx *ctx) {
    if (!ctx) return HFSSS_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->current_phase = BOOT_PHASE_0_HW_INIT;
    ctx->active_slot   = 0xFF;

    /* Populate default slot descriptors (sim: both valid, A preferred) */
    ctx->slots[0].magic   = NOR_SLOT_MAGIC;
    ctx->slots[0].version = 1;
    ctx->slots[0].active  = 1;
    ctx->slots[1].magic   = NOR_SLOT_MAGIC;
    ctx->slots[1].version = 0;
    ctx->slots[1].active  = 0;

    /* Default SysInfo: treat as first boot until caller loads from NOR */
    memset(&ctx->sysinfo, 0xFF, sizeof(ctx->sysinfo));

    pthread_mutex_init(&ctx->lock, NULL);
    return HFSSS_OK;
}

void boot_ctx_cleanup(struct boot_ctx *ctx) {
    if (!ctx) return;
    pthread_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

int boot_run(struct boot_ctx *ctx) {
    if (!ctx) return HFSSS_ERR_INVAL;

    ctx->boot_start_ns = get_ns();

    typedef int (*phase_fn)(struct boot_ctx *);
    static const phase_fn phases[BOOT_PHASE_COUNT] = {
        phase0_hw_init,
        phase1_post,
        phase2_meta_load,
        phase3_ctrl_init,
        phase4_nvme_init,
        phase5_ready,
    };
    static const char *phase_names[BOOT_PHASE_COUNT] = {
        "HW Init", "POST", "Meta Load", "Ctrl Init", "NVMe Init", "Ready"
    };

    /* Stamp power cycle count before phases */
    if (boot_sysinfo_verify(&ctx->sysinfo) == HFSSS_OK) {
        ctx->sysinfo.boot_count++;
        ctx->sysinfo.crc32 = hfsss_crc32(&ctx->sysinfo,
                                            sizeof(ctx->sysinfo) -
                                            sizeof(ctx->sysinfo.crc32));
    } else if (ctx->sysinfo.magic == 0xFFFFFFFFu) {
        /* Blank NOR — initialize sysinfo cleanly before first-boot stamp */
        boot_sysinfo_init(&ctx->sysinfo);
    }

    for (int i = 0; i < BOOT_PHASE_COUNT; i++) {
        ctx->current_phase  = (enum boot_phase)i;
        ctx->phase_start_ns[i] = get_ns();

        int ret = phases[i](ctx);
        if (ret != HFSSS_OK) {
            char msg[BOOT_LOG_MSG_LEN];
            snprintf(msg, sizeof(msg), "Phase %d (%s) FAILED: %d",
                     i, phase_names[i], ret);
            boot_log(ctx, 2, msg);
            return ret;
        }

        ctx->phase_dur_ns[i] = get_ns() - ctx->phase_start_ns[i];

        char msg[BOOT_LOG_MSG_LEN];
        snprintf(msg, sizeof(msg), "Phase %d (%s) OK in %llu ms",
                 i, phase_names[i],
                 (unsigned long long)(ctx->phase_dur_ns[i] / 1000000ULL));
        boot_log(ctx, 0, msg);
    }

    boot_sysinfo_stamp_boot(&ctx->sysinfo, ctx->boot_type);
    ctx->initialized = true;
    return HFSSS_OK;
}

enum boot_type boot_get_type(const struct boot_ctx *ctx) {
    if (!ctx) return BOOT_FIRST;
    return ctx->boot_type;
}

/* ------------------------------------------------------------------
 * Power management
 * ------------------------------------------------------------------ */
int power_mgmt_init(struct power_mgmt_ctx *ctx, struct boot_ctx *boot) {
    if (!ctx || !boot) return HFSSS_ERR_INVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->boot = boot;
    pthread_mutex_init(&ctx->lock, NULL);
    return HFSSS_OK;
}

void power_mgmt_cleanup(struct power_mgmt_ctx *ctx) {
    if (!ctx) return;
    pthread_mutex_destroy(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

int power_mgmt_normal_shutdown(struct power_mgmt_ctx *ctx) {
    if (!ctx) return HFSSS_ERR_INVAL;

    pthread_mutex_lock(&ctx->lock);
    ctx->shutdown_in_progress = true;
    ctx->last_shutdown_type   = SHUTDOWN_NORMAL;
    pthread_mutex_unlock(&ctx->lock);

    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 1/7 — halt I/O acceptance");
    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 2/7 — flush write buffer");
    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 3/7 — complete GC cycle");
    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 4/7 — write L2P checkpoint");
    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 5/7 — flush WAL and sync");
    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 6/7 — write SysInfo clean marker");
    HFSSS_LOG_INFO("POWER", "Normal shutdown: step 7/7 — CSTS.SHST=0x02");

    if (ctx->boot) {
        struct sysinfo_partition *si = &ctx->boot->sysinfo;
        si->last_shutdown_type            = (uint8_t)SHUTDOWN_NORMAL;
        si->clean_shutdown_marker_valid   = 1;
        si->clean_shutdown_marker         = SYSINFO_CLEAN_MARKER;
        si->crash_marker_valid            = 0;
        si->crash_marker                  = 0;
        si->last_shutdown_ns              = get_realtime_ns();
        si->crc32 = hfsss_crc32(si, sizeof(*si) - sizeof(si->crc32));
    }

    return HFSSS_OK;
}

void power_mgmt_abnormal_shutdown(struct power_mgmt_ctx *ctx,
                                   enum shutdown_type type) {
    if (!ctx) return;

    /* Signal handler context — no heap allocation, no locks */
    ctx->shutdown_in_progress = true;
    ctx->crash_marker_written = true;
    ctx->last_shutdown_type   = type;

    if (ctx->boot) {
        struct sysinfo_partition *si = &ctx->boot->sysinfo;
        si->last_shutdown_type      = (uint8_t)type;
        si->crash_marker_valid      = 1;
        si->crash_marker            = SYSINFO_CRASH_MARKER;
        si->clean_shutdown_marker_valid = 0;
        si->last_shutdown_ns        = get_realtime_ns();
        if (type != SHUTDOWN_NORMAL) {
            si->unsafe_shutdown_count++;
        }
        /* CRC update — allowed since we own the struct */
        si->crc32 = hfsss_crc32(si, sizeof(*si) - sizeof(si->crc32));
    }
}

int power_mgmt_recover_wal(struct power_mgmt_ctx *ctx, const char *wal_path) {
    if (!ctx || !wal_path) return HFSSS_ERR_INVAL;

    FILE *f = fopen(wal_path, "rb");
    if (!f) {
        HFSSS_LOG_WARN("POWER", "WAL file not found: %s", wal_path);
        return HFSSS_ERR_NOENT;
    }

    /* WAL record format: 64 bytes fixed.
     * Bytes [0-3]:  magic  0x12345678
     * Bytes [4-7]:  type
     * Bytes [8-15]: sequence
     * Bytes [56-59]: CRC32
     * Bytes [60-63]: end marker 0xDEADBEEF */
    uint8_t record[64];
    uint32_t replayed = 0;

    while (fread(record, 1, 64, f) == 64) {
        uint32_t magic;
        memcpy(&magic, record, 4);
        if (magic != 0x12345678u) {
            break;
        }
        uint32_t end_marker;
        memcpy(&end_marker, record + 60, 4);
        if (end_marker == 0xDEADBEEFu) {
            break;
        }
        replayed++;
    }
    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "WAL recovery: replayed %u records from %s",
             replayed, wal_path);
    HFSSS_LOG_INFO("POWER", "%s", msg);

    return HFSSS_OK;
}
