#ifndef __HFSSS_HAL_PCI_H
#define __HFSSS_HAL_PCI_H

#include <stdint.h>
#include "common/common.h"
#include "common/mutex.h"
#include "common/msgqueue.h"

#define HAL_PCI_MAX_NAMESPACES 32
#define HAL_PCI_CQ_SIZE 256

/* ------------------------------------------------------------------
 * REQ-069: byte-level PCI/PCIe config space emulation per LLD_13.
 *
 * The hal_pci_cfg object is a pure byte buffer sized for the PCIe
 * Extended Configuration Space (256B Type 0 header + 3840B extended
 * = 4KB total). Read/write helpers enforce PCIe bus semantics:
 *   - Out-of-range reads return all-ones (0xFF / 0xFFFF / 0xFFFFFFFF)
 *     mimicking an unresponsive device.
 *   - 16/32-bit reads require natural alignment; unaligned reads
 *     return all-ones.
 * Standard vendor/device/class/status fields + the capabilities
 * pointer are stamped at init so the capability-walk (REQ-002)
 * layered on top of these helpers sees a coherent starting state.
 * ------------------------------------------------------------------ */

#define PCI_CFG_SPACE_SIZE      256u
#define PCI_EXT_CFG_SPACE_SIZE  4096u

struct hal_pci_cfg {
    uint8_t cfg[PCI_EXT_CFG_SPACE_SIZE];
    bool    initialized;
};

int      hal_pci_cfg_init   (struct hal_pci_cfg *cfg);
uint8_t  hal_pci_cfg_read8  (const struct hal_pci_cfg *cfg, uint32_t offset);
uint16_t hal_pci_cfg_read16 (const struct hal_pci_cfg *cfg, uint32_t offset);
uint32_t hal_pci_cfg_read32 (const struct hal_pci_cfg *cfg, uint32_t offset);
int hal_pci_cfg_write8 (struct hal_pci_cfg *cfg, uint32_t offset, uint8_t  val);
int hal_pci_cfg_write16(struct hal_pci_cfg *cfg, uint32_t offset, uint16_t val);
int hal_pci_cfg_write32(struct hal_pci_cfg *cfg, uint32_t offset, uint32_t val);

/* REQ-002: walk the PCIe capability linked list (chain head at
 * cfg_space[0x34], each entry: [cap_id, next]). Returns the
 * offset of the first entry matching `cap_id`, or 0 when not
 * found. The walker self-limits to a safe hop count so a
 * corrupted next-pointer cannot cause an infinite loop. */
uint8_t hal_pci_capability_find(const struct hal_pci_cfg *cfg, uint8_t cap_id);

/* Command Completion Entry */
struct hal_pci_completion {
    u32 command_id;
    u32 status;
    u64 result;
    u64 timestamp;
};

/* Namespace Information */
struct hal_pci_namespace {
    bool active;
    u32 nsid;
    u64 size;       /* in logical blocks */
    u32 lba_size;   /* logical block size */
    bool write_protected;
    char name[32];
};

/* HAL PCI Context */
struct hal_pci_ctx {
    bool initialized;
    struct mutex lock;

    /* Command completion queue */
    struct msg_queue cq;
    struct hal_pci_completion cq_entries[HAL_PCI_CQ_SIZE];
    u32 cq_head;
    u32 cq_tail;

    /* Namespace management */
    struct hal_pci_namespace namespaces[HAL_PCI_MAX_NAMESPACES];
    u32 active_namespace_count;
};

/* Function Prototypes */
int hal_pci_init(struct hal_pci_ctx *ctx);
void hal_pci_cleanup(struct hal_pci_ctx *ctx);

/* Command completion submission (REQ-062) */
int hal_pci_submit_completion(struct hal_pci_ctx *ctx, const struct hal_pci_completion *comp);
int hal_pci_poll_completion(struct hal_pci_ctx *ctx, struct hal_pci_completion *comp);
int hal_pci_peek_completion(struct hal_pci_ctx *ctx, struct hal_pci_completion *comp);
u32 hal_pci_get_completion_count(struct hal_pci_ctx *ctx);

/* Namespace management (REQ-065) */
int hal_pci_ns_attach(struct hal_pci_ctx *ctx, u32 nsid, u64 size, u32 lba_size);
int hal_pci_ns_detach(struct hal_pci_ctx *ctx, u32 nsid);
int hal_pci_ns_get_info(struct hal_pci_ctx *ctx, u32 nsid, struct hal_pci_namespace *info);
int hal_pci_ns_set_write_protect(struct hal_pci_ctx *ctx, u32 nsid, bool wp);
int hal_pci_ns_list(struct hal_pci_ctx *ctx, u32 *nsid_list, u32 *count);
u32 hal_pci_ns_get_active_count(struct hal_pci_ctx *ctx);
int hal_pci_ns_format(struct hal_pci_ctx *ctx, u32 nsid, u32 new_lba_size);

#endif /* __HFSSS_HAL_PCI_H */
