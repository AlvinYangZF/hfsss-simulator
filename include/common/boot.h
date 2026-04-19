#ifndef HFSSS_COMMON_BOOT_H
#define HFSSS_COMMON_BOOT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "common/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Boot sequence phases (REQ-078)
 * ------------------------------------------------------------------ */
enum boot_phase {
    BOOT_PHASE_0_HW_INIT   = 0,  /* NAND/NOR init, memory pools */
    BOOT_PHASE_1_POST      = 1,  /* POST self-test, NOR slot CRC */
    BOOT_PHASE_2_META_LOAD = 2,  /* BBT load, L2P checkpoint, WAL replay */
    BOOT_PHASE_3_CTRL_INIT = 3,  /* controller arbitration, WB, cache */
    BOOT_PHASE_4_NVME_INIT = 4,  /* NVMe queue structs, CSTS.RDY=1 */
    BOOT_PHASE_5_READY     = 5,  /* OOB listener, monitoring, I/O open */
    BOOT_PHASE_COUNT       = 6,
};

/* Boot type detected from SysInfo at power-on (REQ-080) */
enum boot_type {
    BOOT_FIRST    = 0,  /* blank SysInfo: first power-on, format */
    BOOT_NORMAL   = 1,  /* clean_shutdown marker present */
    BOOT_RECOVERY = 2,  /* crash marker or missing clean marker */
    BOOT_DEGRADED = 3,  /* WAL replay succeeded with warnings */
};

/* Classification of last shutdown event (REQ-081) */
enum shutdown_type {
    SHUTDOWN_NORMAL     = 0,  /* NVMe shutdown notification */
    SHUTDOWN_ABNORMAL   = 1,  /* SIGTERM/SIGINT, best-effort flush */
    SHUTDOWN_POWER_LOSS = 2,  /* SIGKILL / sudden power loss */
};

/* ------------------------------------------------------------------
 * NOR firmware slot descriptor (REQ-079) — 64 bytes, packed
 * ------------------------------------------------------------------ */
#define NOR_SLOT_MAGIC           0x48465353U  /* "HFSS" */
#define NOR_SLOT_VERSION_INVALID 0xFFFFFFFFU

struct nor_firmware_slot {
    uint32_t magic;            /* NOR_SLOT_MAGIC or 0xFFFFFFFF */
    uint32_t version;          /* firmware build number */
    uint32_t crc32;            /* CRC-32 of firmware image bytes */
    uint8_t  active;           /* 1 = primary boot candidate */
    uint8_t  reserved0[3];
    uint32_t image_size;       /* firmware image bytes */
    uint32_t build_timestamp;  /* Unix epoch seconds */
    uint8_t  reserved1[40];
} __attribute__((packed));     /* 64 bytes */

/* ------------------------------------------------------------------
 * SysInfo partition (REQ-080, REQ-081) — 128 bytes, packed
 * ------------------------------------------------------------------ */
#define SYSINFO_MAGIC          0x53594E46U  /* "SYNF" */
#define SYSINFO_CLEAN_MARKER   0xC1EA4E00U
#define SYSINFO_CRASH_MARKER   0xCCA5DEADU

struct sysinfo_partition {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t unsafe_shutdown_count;
    uint8_t  last_shutdown_type;
    uint8_t  clean_shutdown_marker_valid;
    uint8_t  crash_marker_valid;
    uint8_t  reserved0;
    uint32_t clean_shutdown_marker;
    uint32_t crash_marker;
    uint64_t last_shutdown_ns;
    uint64_t last_boot_ns;
    uint64_t total_power_on_ns;
    uint32_t wal_sequence_at_shutdown;
    uint32_t checkpoint_seq_at_shutdown;
    uint8_t  active_slot;
    uint8_t  boot_type_last;
    uint8_t  reserved1[50];
    uint32_t crc32;
} __attribute__((packed));    /* 128 bytes */

/* ------------------------------------------------------------------
 * Boot context (REQ-078, REQ-079)
 * ------------------------------------------------------------------ */
#define BOOT_LOG_ENTRY_MAX  64
#define BOOT_LOG_MSG_LEN    128

struct boot_log_entry {
    uint64_t timestamp_ns;
    uint8_t  phase;
    uint8_t  level;           /* 0=INFO 1=WARN 2=ERROR */
    char     msg[BOOT_LOG_MSG_LEN];
};

/* ------------------------------------------------------------------
 * Firmware signature / secure boot chain verification (REQ-164)
 *
 * `secure_boot_verify()` is defined here in common — its dependencies
 * are just hfsss_crc32 + fixed-width types. Controller/security re-exports
 * these symbols via `controller/security.h` so the security module's
 * callers don't have to change their include lists.
 * ------------------------------------------------------------------ */
#define FW_SIG_MAGIC 0x46575347U  /* "FWSG" */

struct fw_signature {
    uint32_t magic;
    uint32_t fw_version;
    uint32_t image_crc32;
    uint32_t reserved;
};

bool secure_boot_verify(const uint8_t *image, uint32_t size,
                        const struct fw_signature *sig);

struct boot_ctx {
    enum boot_phase      current_phase;
    enum boot_type       boot_type;
    struct sysinfo_partition sysinfo;
    struct nor_firmware_slot slots[2];
    uint8_t              active_slot;    /* 0=A, 1=B */
    bool                 initialized;
    uint64_t             boot_start_ns;
    uint64_t             phase_start_ns[BOOT_PHASE_COUNT];
    uint64_t             phase_dur_ns[BOOT_PHASE_COUNT];
    struct boot_log_entry log[BOOT_LOG_ENTRY_MAX];
    uint32_t             log_count;
    pthread_mutex_t      lock;
    /* Secure boot attestation inputs (REQ-164). Optional: when NULL the
     * POST phase skips image verification and only validates the slot
     * header. When set, POST invokes secure_boot_verify() and aborts on
     * mismatch. */
    const uint8_t              *fw_image;
    uint32_t                    fw_image_size;
    const struct fw_signature  *fw_sig;
};

/* ------------------------------------------------------------------
 * Power management context (REQ-081)
 * ------------------------------------------------------------------ */
struct power_mgmt_ctx {
    bool             shutdown_in_progress;
    bool             crash_marker_written;
    enum shutdown_type last_shutdown_type;
    struct boot_ctx *boot;
    pthread_mutex_t  lock;
};

/* ------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------ */

/* Boot sequence */
int          boot_ctx_init(struct boot_ctx *ctx);
void         boot_ctx_cleanup(struct boot_ctx *ctx);
int          boot_run(struct boot_ctx *ctx);
enum boot_type boot_get_type(const struct boot_ctx *ctx);

/* Firmware slot management (REQ-079) */
int  boot_select_firmware_slot(struct boot_ctx *ctx);
int  boot_swap_firmware_slot(struct boot_ctx *ctx);
bool boot_slot_valid(const struct nor_firmware_slot *slot);

/* Secure boot attestation input (REQ-164).
 * Attach a firmware image + signature pair so POST runs
 * secure_boot_verify() and aborts the boot sequence on mismatch. */
void boot_attach_fw_image(struct boot_ctx *ctx,
                          const uint8_t *image, uint32_t size,
                          const struct fw_signature *sig);

/* SysInfo helpers */
void boot_sysinfo_init(struct sysinfo_partition *si);
int  boot_sysinfo_verify(const struct sysinfo_partition *si);
void boot_sysinfo_stamp_boot(struct sysinfo_partition *si, enum boot_type bt);

/* Power management */
int  power_mgmt_init(struct power_mgmt_ctx *ctx, struct boot_ctx *boot);
void power_mgmt_cleanup(struct power_mgmt_ctx *ctx);
int  power_mgmt_normal_shutdown(struct power_mgmt_ctx *ctx);
void power_mgmt_abnormal_shutdown(struct power_mgmt_ctx *ctx,
                                  enum shutdown_type type);
int  power_mgmt_recover_wal(struct power_mgmt_ctx *ctx,
                             const char *wal_path);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_COMMON_BOOT_H */
