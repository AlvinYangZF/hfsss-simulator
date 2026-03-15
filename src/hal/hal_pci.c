#include "hal/hal_pci.h"
#include <stdio.h>
#include <string.h>

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
