#include "hal/hal_pci.h"
#include "pcie/pci.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * REQ-069: byte-level PCI/PCIe config space (LLD_13 §6.3).
 *
 * Internal helper: classify an access request. Returns true if the
 * (offset, size) pair is serviceable; false on any of:
 *   - offset is outside the 4 KB window
 *   - the remaining bytes at `offset` can't fit `size`
 *   - 16/32-bit access that isn't naturally aligned
 *
 * Bounds are checked BEFORE `offset + size` so a u32 near UINT32_MAX
 * can't silently wrap back into the valid range and index the
 * backing buffer far past the 4 KB window.
 * ------------------------------------------------------------------ */
static inline bool cfg_access_ok(uint32_t offset, uint32_t size)
{
    if (size == 2 && (offset & 0x1)) return false;
    if (size == 4 && (offset & 0x3)) return false;
    if (offset >= PCI_EXT_CFG_SPACE_SIZE) return false;
    if (PCI_EXT_CFG_SPACE_SIZE - offset < size) return false;
    return true;
}

/* PCI Type 0 header fields that are read-only from the host side.
 * Matches `src/pcie/pci.c::pci_dev_cfg_write` so both config-space
 * surfaces enforce the same spec-level invariants. A write landing
 * on any byte of a RO field is silently dropped (PCIe §7.5.1.1:
 * "Writes to Read-Only registers shall complete normally without
 * modifying the register"). Returns true if [off, off+size) overlaps
 * any read-only byte. */
static bool cfg_range_is_readonly(uint32_t offset, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        uint32_t b = offset + i;
        if (b == 0x00 || b == 0x01) return true; /* Vendor ID */
        if (b == 0x02 || b == 0x03) return true; /* Device ID */
        if (b == 0x08)              return true; /* Revision ID */
        if (b >= 0x09 && b <= 0x0B) return true; /* Class code */
        if (b == 0x0E)              return true; /* Header type */
        if (b == 0x2C || b == 0x2D) return true; /* Subsystem Vendor ID */
        if (b == 0x2E || b == 0x2F) return true; /* Subsystem ID */
        if (b == 0x34)              return true; /* Capabilities pointer */
        if (b >= 0x3D && b <= 0x3F) return true; /* Interrupt pin / GNT / LAT */
    }
    return false;
}

int hal_pci_cfg_init(struct hal_pci_cfg *cfg)
{
    if (!cfg) {
        return HFSSS_ERR_INVAL;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* Vendor / Device / Revision */
    cfg->cfg[0x00] = (uint8_t)(HFSSS_VENDOR_ID & 0xFF);
    cfg->cfg[0x01] = (uint8_t)(HFSSS_VENDOR_ID >> 8);
    cfg->cfg[0x02] = (uint8_t)(HFSSS_DEVICE_ID & 0xFF);
    cfg->cfg[0x03] = (uint8_t)(HFSSS_DEVICE_ID >> 8);
    cfg->cfg[0x08] = HFSSS_REVISION_ID;

    /* Class code: byte 0x09 = programming interface, 0x0A = subclass,
     * 0x0B = base class. NVMe = base 0x01 (storage), sub 0x08, IF 0x02. */
    cfg->cfg[0x09] = PCI_CLASS_INTERFACE_NVME;
    cfg->cfg[0x0A] = PCI_CLASS_SUBCLASS_NVME;
    cfg->cfg[0x0B] = PCI_CLASS_CODE_STORAGE;

    /* Header type = Type 0 Endpoint. */
    cfg->cfg[0x0E] = PCI_HEADER_TYPE_NORMAL;

    /* Status: Capabilities list present so the host walks the chain. */
    cfg->cfg[0x06] = (uint8_t)(PCI_STS_CAP_LIST & 0xFF);
    cfg->cfg[0x07] = (uint8_t)(PCI_STS_CAP_LIST >> 8);

    /* Subsystem IDs mirror vendor/device. */
    cfg->cfg[0x2C] = (uint8_t)(HFSSS_SUBSYSTEM_VENDOR_ID & 0xFF);
    cfg->cfg[0x2D] = (uint8_t)(HFSSS_SUBSYSTEM_VENDOR_ID >> 8);
    cfg->cfg[0x2E] = (uint8_t)(HFSSS_SUBSYSTEM_ID & 0xFF);
    cfg->cfg[0x2F] = (uint8_t)(HFSSS_SUBSYSTEM_ID >> 8);

    /* Capabilities pointer -> first capability at 0x40. */
    cfg->cfg[0x34] = 0x40;

    /* Interrupt pin: INTA#. */
    cfg->cfg[0x3D] = 0x01;

    /* ------------------------------------------------------------------
     * REQ-002: PCIe capability linked list seeded in-place.
     *   0x40 : Power Management  -> 0x50
     *   0x50 : MSI                -> 0x70
     *   0x70 : MSI-X              -> 0x90
     *   0x90 : PCI Express        -> 0x00 (end of list)
     * Each header is the standard { cap_id, next_ptr } pair; the
     * payload bytes are stamped to match LLD_01 §3.2 defaults so the
     * host sees a coherent capability on read.
     * ------------------------------------------------------------------ */
    /* PM @ 0x40: PMC = 0x0003 (v1.2), PMCSR D0 */
    cfg->cfg[0x40] = PCI_CAP_ID_PM;
    cfg->cfg[0x41] = 0x50;
    cfg->cfg[0x42] = 0x03; cfg->cfg[0x43] = 0x00;

    /* MSI @ 0x50: Message Control = 0x0080 (64-bit addressing capable) */
    cfg->cfg[0x50] = PCI_CAP_ID_MSI;
    cfg->cfg[0x51] = 0x70;
    cfg->cfg[0x52] = 0x80; cfg->cfg[0x53] = 0x00;

    /* MSI-X @ 0x70: Message Control = 0x001F (32 vectors).
     * Table Offset / BIR (cap+0x04): bits [2:0] = BIR, [31:3] = offset.
     *   Table = BAR2 + 0x4000  -> 0x00004002
     *   PBA   = BAR4 + 0x8000  -> 0x00008004
     * Previously both had BIR=0 which pointed at BAR0 despite the
     * "BAR2/BAR4" comment — caught by the PR #93 self-review. */
    cfg->cfg[0x70] = PCI_CAP_ID_MSIX;
    cfg->cfg[0x71] = 0x90;
    cfg->cfg[0x72] = 0x1F; cfg->cfg[0x73] = 0x00;
    cfg->cfg[0x74] = 0x02; cfg->cfg[0x75] = 0x40;
    cfg->cfg[0x76] = 0x00; cfg->cfg[0x77] = 0x00;
    cfg->cfg[0x78] = 0x04; cfg->cfg[0x79] = 0x80;
    cfg->cfg[0x7A] = 0x00; cfg->cfg[0x7B] = 0x00;

    /* PCI Express @ 0x90: PCIE_CAP = 0x0002 (Express Endpoint v2),
     * terminator next = 0x00. */
    cfg->cfg[0x90] = PCI_CAP_ID_EXP;
    cfg->cfg[0x91] = 0x00;
    cfg->cfg[0x92] = 0x02; cfg->cfg[0x93] = 0x00;

    cfg->initialized = true;
    return HFSSS_OK;
}

uint8_t hal_pci_capability_find(const struct hal_pci_cfg *cfg, uint8_t cap_id)
{
    if (!cfg) {
        return 0;
    }
    /* Capabilities pointer lives at PCI header offset 0x34. A value
     * of 0 or 0xFF both indicate "no more entries". Cap headers
     * start at 0x40, so a properly-formed list never produces 0x34
     * itself as a next. */
    uint8_t off = hal_pci_cfg_read8(cfg, 0x34);

    /* Each cap occupies at least 2 bytes; the whole standard header
     * region after 0x40 is 192 bytes, so an upper bound of 96 hops
     * covers any legal chain. Treat anything beyond that as a
     * corrupted (possibly cyclic) pointer and bail. */
    for (int hops = 0; hops < 96; hops++) {
        if (off == 0 || off == 0xFF) {
            return 0;
        }
        /* Standard caps live in [0x40, 0x100). `off` is 1 byte so
         * the upper bound is enforced by its type; we only need to
         * reject pointers that land in the PCI header region. */
        if (off < 0x40) {
            return 0;
        }
        if (hal_pci_cfg_read8(cfg, off) == cap_id) {
            return off;
        }
        off = hal_pci_cfg_read8(cfg, off + 1);
    }
    return 0;
}

uint8_t hal_pci_cfg_read8(const struct hal_pci_cfg *cfg, uint32_t offset)
{
    if (!cfg || !cfg_access_ok(offset, 1)) {
        return 0xFF;
    }
    return cfg->cfg[offset];
}

uint16_t hal_pci_cfg_read16(const struct hal_pci_cfg *cfg, uint32_t offset)
{
    if (!cfg || !cfg_access_ok(offset, 2)) {
        return 0xFFFF;
    }
    /* Assemble little-endian so the layout matches what a
     * memory-mapped PCI register would return on x86 / arm. */
    return (uint16_t)cfg->cfg[offset]
         | ((uint16_t)cfg->cfg[offset + 1] << 8);
}

uint32_t hal_pci_cfg_read32(const struct hal_pci_cfg *cfg, uint32_t offset)
{
    if (!cfg || !cfg_access_ok(offset, 4)) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)cfg->cfg[offset]
         | ((uint32_t)cfg->cfg[offset + 1] << 8)
         | ((uint32_t)cfg->cfg[offset + 2] << 16)
         | ((uint32_t)cfg->cfg[offset + 3] << 24);
}

int hal_pci_cfg_write8(struct hal_pci_cfg *cfg, uint32_t offset, uint8_t val)
{
    if (!cfg || !cfg_access_ok(offset, 1)) {
        return HFSSS_ERR_INVAL;
    }
    if (cfg_range_is_readonly(offset, 1)) {
        return HFSSS_OK;  /* silent drop per PCIe §7.5.1.1 */
    }
    cfg->cfg[offset] = val;
    return HFSSS_OK;
}

int hal_pci_cfg_write16(struct hal_pci_cfg *cfg, uint32_t offset, uint16_t val)
{
    if (!cfg || !cfg_access_ok(offset, 2)) {
        return HFSSS_ERR_INVAL;
    }
    if (cfg_range_is_readonly(offset, 2)) {
        return HFSSS_OK;
    }
    cfg->cfg[offset]     = (uint8_t)(val & 0xFF);
    cfg->cfg[offset + 1] = (uint8_t)(val >> 8);
    return HFSSS_OK;
}

int hal_pci_cfg_write32(struct hal_pci_cfg *cfg, uint32_t offset, uint32_t val)
{
    if (!cfg || !cfg_access_ok(offset, 4)) {
        return HFSSS_ERR_INVAL;
    }
    if (cfg_range_is_readonly(offset, 4)) {
        return HFSSS_OK;
    }
    cfg->cfg[offset]     = (uint8_t)(val & 0xFF);
    cfg->cfg[offset + 1] = (uint8_t)((val >> 8) & 0xFF);
    cfg->cfg[offset + 2] = (uint8_t)((val >> 16) & 0xFF);
    cfg->cfg[offset + 3] = (uint8_t)((val >> 24) & 0xFF);
    return HFSSS_OK;
}

int hal_pci_init(struct hal_pci_ctx *ctx)
{
    int ret;

    if (!ctx) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize command completion queue */
    ret = msg_queue_init(&ctx->cq, sizeof(struct hal_pci_completion), HAL_PCI_CQ_SIZE);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&ctx->lock);
        memset(ctx, 0, sizeof(*ctx));
        return ret;
    }

    ctx->cq_head = 0;
    ctx->cq_tail = 0;

    /* Initialize namespaces */
    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        ctx->namespaces[i].active = false;
        ctx->namespaces[i].nsid = 0;
    }
    ctx->active_namespace_count = 0;

    ctx->initialized = true;
    return HFSSS_OK;
}

void hal_pci_cleanup(struct hal_pci_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    mutex_lock(&ctx->lock, 0);

    msg_queue_cleanup(&ctx->cq);

    ctx->initialized = false;
    ctx->active_namespace_count = 0;

    mutex_unlock(&ctx->lock);
    mutex_cleanup(&ctx->lock);

    memset(ctx, 0, sizeof(*ctx));
}

/* Command completion submission (REQ-062) */
int hal_pci_submit_completion(struct hal_pci_ctx *ctx, const struct hal_pci_completion *comp)
{
    if (!ctx || !ctx->initialized || !comp) {
        return HFSSS_ERR_INVAL;
    }

    int ret = msg_queue_trysend(&ctx->cq, comp);
    if (ret != HFSSS_OK) {
        return ret;
    }

    mutex_lock(&ctx->lock, 0);
    ctx->cq_entries[ctx->cq_tail] = *comp;
    ctx->cq_tail = (ctx->cq_tail + 1) % HAL_PCI_CQ_SIZE;
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int hal_pci_poll_completion(struct hal_pci_ctx *ctx, struct hal_pci_completion *comp)
{
    if (!ctx || !ctx->initialized || !comp) {
        return HFSSS_ERR_INVAL;
    }

    int ret = msg_queue_tryrecv(&ctx->cq, comp);
    if (ret == HFSSS_OK) {
        mutex_lock(&ctx->lock, 0);
        ctx->cq_head = (ctx->cq_head + 1) % HAL_PCI_CQ_SIZE;
        mutex_unlock(&ctx->lock);
    }

    return ret;
}

int hal_pci_peek_completion(struct hal_pci_ctx *ctx, struct hal_pci_completion *comp)
{
    if (!ctx || !ctx->initialized || !comp) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);
    if (ctx->cq_head == ctx->cq_tail) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_AGAIN;
    }
    *comp = ctx->cq_entries[ctx->cq_head];
    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

u32 hal_pci_get_completion_count(struct hal_pci_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    return msg_queue_count(&ctx->cq);
}

/* Namespace management (REQ-065) */
int hal_pci_ns_attach(struct hal_pci_ctx *ctx, u32 nsid, u64 size, u32 lba_size)
{
    if (!ctx || !ctx->initialized || nsid == 0 || size == 0 || lba_size == 0) {
        return HFSSS_ERR_INVAL;
    }

    /* Check if lba_size is a power of 2 and at least 512 */
    if ((lba_size < 512) || ((lba_size & (lba_size - 1)) != 0)) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    /* Check if NSID already exists */
    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        if (ctx->namespaces[i].active && ctx->namespaces[i].nsid == nsid) {
            mutex_unlock(&ctx->lock);
            return HFSSS_ERR_EXIST;
        }
    }

    /* Find a free slot */
    int free_slot = -1;
    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        if (!ctx->namespaces[i].active) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOMEM;
    }

    /* Initialize namespace */
    ctx->namespaces[free_slot].active = true;
    ctx->namespaces[free_slot].nsid = nsid;
    ctx->namespaces[free_slot].size = size;
    ctx->namespaces[free_slot].lba_size = lba_size;
    ctx->namespaces[free_slot].write_protected = false;
    snprintf(ctx->namespaces[free_slot].name, sizeof(ctx->namespaces[free_slot].name), "Namespace %u", nsid);
    ctx->active_namespace_count++;

    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

int hal_pci_ns_detach(struct hal_pci_ctx *ctx, u32 nsid)
{
    if (!ctx || !ctx->initialized || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        if (ctx->namespaces[i].active && ctx->namespaces[i].nsid == nsid) {
            ctx->namespaces[i].active = false;
            ctx->namespaces[i].nsid = 0;
            ctx->active_namespace_count--;
            mutex_unlock(&ctx->lock);
            return HFSSS_OK;
        }
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_ERR_NOENT;
}

int hal_pci_ns_get_info(struct hal_pci_ctx *ctx, u32 nsid, struct hal_pci_namespace *info)
{
    if (!ctx || !ctx->initialized || !info || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        if (ctx->namespaces[i].active && ctx->namespaces[i].nsid == nsid) {
            *info = ctx->namespaces[i];
            mutex_unlock(&ctx->lock);
            return HFSSS_OK;
        }
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_ERR_NOENT;
}

int hal_pci_ns_set_write_protect(struct hal_pci_ctx *ctx, u32 nsid, bool wp)
{
    if (!ctx || !ctx->initialized || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        if (ctx->namespaces[i].active && ctx->namespaces[i].nsid == nsid) {
            ctx->namespaces[i].write_protected = wp;
            mutex_unlock(&ctx->lock);
            return HFSSS_OK;
        }
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_ERR_NOENT;
}

int hal_pci_ns_list(struct hal_pci_ctx *ctx, u32 *nsid_list, u32 *count)
{
    if (!ctx || !ctx->initialized || !count) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    u32 max_count = *count;
    *count = 0;

    for (u32 i = 0; i < HAL_PCI_MAX_NAMESPACES && *count < max_count; i++) {
        if (ctx->namespaces[i].active) {
            if (nsid_list) {
                nsid_list[*count] = ctx->namespaces[i].nsid;
            }
            (*count)++;
        }
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_OK;
}

u32 hal_pci_ns_get_active_count(struct hal_pci_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }

    return ctx->active_namespace_count;
}

int hal_pci_ns_format(struct hal_pci_ctx *ctx, u32 nsid, u32 new_lba_size)
{
    u32 i;

    if (!ctx || !ctx->initialized || nsid == 0) {
        return HFSSS_ERR_INVAL;
    }

    if (new_lba_size != 0 && new_lba_size != 512 &&
        new_lba_size != 4096 && new_lba_size != 16384) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    for (i = 0; i < HAL_PCI_MAX_NAMESPACES; i++) {
        if (ctx->namespaces[i].active && ctx->namespaces[i].nsid == nsid) {
            if (new_lba_size > 0) {
                ctx->namespaces[i].lba_size = new_lba_size;
            }
            /* Format resets the namespace to a clean state.
             * The caller is responsible for clearing FTL-level
             * mapping tables separately. */
            mutex_unlock(&ctx->lock);
            return HFSSS_OK;
        }
    }

    mutex_unlock(&ctx->lock);
    return HFSSS_ERR_NOENT;
}
