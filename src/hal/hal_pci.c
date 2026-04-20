#include "hal/hal_pci.h"
#include "pcie/pci.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * REQ-069: byte-level PCI/PCIe config space (LLD_13 §6.3).
 *
 * Internal helper: classify an access request. Returns true if the
 * (offset, size) pair is serviceable; false on any of:
 *   - offset + size wraps past PCI_EXT_CFG_SPACE_SIZE
 *   - 16/32-bit access that isn't naturally aligned
 * Out-of-range or misaligned requests translate at the public API
 * to a PCIe "all-ones" response on read, and HFSSS_ERR_INVAL on
 * write, mirroring an unresponsive device.
 * ------------------------------------------------------------------ */
static inline bool cfg_access_ok(uint32_t offset, uint32_t size)
{
    if (size == 2 && (offset & 0x1)) return false;
    if (size == 4 && (offset & 0x3)) return false;
    if (offset + size > PCI_EXT_CFG_SPACE_SIZE) return false;
    return true;
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

    /* Capabilities pointer -> first capability at 0x40. The actual
     * cap chain is seeded by REQ-002 in a separate step. */
    cfg->cfg[0x34] = 0x40;

    /* Interrupt pin: INTA#. */
    cfg->cfg[0x3D] = 0x01;

    cfg->initialized = true;
    return HFSSS_OK;
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
    cfg->cfg[offset] = val;
    return HFSSS_OK;
}

int hal_pci_cfg_write16(struct hal_pci_cfg *cfg, uint32_t offset, uint16_t val)
{
    if (!cfg || !cfg_access_ok(offset, 2)) {
        return HFSSS_ERR_INVAL;
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
