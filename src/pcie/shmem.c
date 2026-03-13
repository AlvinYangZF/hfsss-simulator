#include "pcie/shmem.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int shmem_create(struct shmem_ctx *ctx, const char *path)
{
    int fd;
    int ret;

    if (!ctx || !path) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Create shared memory file */
    fd = shm_open(path, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (fd < 0) {
        /* Try to unlink and create again */
        shm_unlink(path);
        fd = shm_open(path, O_CREAT | O_RDWR, 0666);
    }

    if (fd < 0) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR;
    }

    /* Set size */
    if (ftruncate(fd, SHMEM_SIZE) < 0) {
        close(fd);
        shm_unlink(path);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR;
    }

    /* Map it */
    void *ptr = mmap(NULL, SHMEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        shm_unlink(path);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR;
    }

    ctx->fd = fd;
    ctx->path = path;
    ctx->layout = (struct shmem_layout *)ptr;
    ctx->size = SHMEM_SIZE;
    ctx->is_producer = true;
    ctx->is_consumer = true;

    /* Initialize header */
    memset(ctx->layout, 0, SHMEM_SIZE);
    ctx->layout->header.magic = RING_BUFFER_MAGIC;
    ctx->layout->header.version = RING_BUFFER_VERSION;
    ctx->layout->header.slot_count = RING_BUFFER_SLOTS;
    ctx->layout->header.slot_size = RING_BUFFER_SLOT_SIZE;
    ctx->layout->header.producer_idx = 0;
    ctx->layout->header.consumer_idx = 0;

    return HFSSS_OK;
}

int shmem_open(struct shmem_ctx *ctx, const char *path)
{
    int fd;
    int ret;

    if (!ctx || !path) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ret = mutex_init(&ctx->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    fd = shm_open(path, O_RDWR, 0666);
    if (fd < 0) {
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR;
    }

    void *ptr = mmap(NULL, SHMEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        mutex_cleanup(&ctx->lock);
        return HFSSS_ERR;
    }

    ctx->fd = fd;
    ctx->path = path;
    ctx->layout = (struct shmem_layout *)ptr;
    ctx->size = SHMEM_SIZE;
    ctx->is_producer = false;
    ctx->is_consumer = true;

    /* Verify magic */
    if (ctx->layout->header.magic != RING_BUFFER_MAGIC) {
        shmem_close(ctx);
        return HFSSS_ERR;
    }

    return HFSSS_OK;
}

void shmem_close(struct shmem_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->layout) {
        munmap(ctx->layout, ctx->size);
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
    }

    mutex_cleanup(&ctx->lock);
    memset(ctx, 0, sizeof(*ctx));
}

int shmem_produce_cmd(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd)
{
    if (!ctx || !cmd || !ctx->is_producer) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    u32 producer = ctx->layout->header.producer_idx;
    u32 consumer = ctx->layout->header.consumer_idx;
    u32 next = (producer + 1) % RING_BUFFER_SLOTS;

    if (next == consumer) {
        /* Buffer full */
        ctx->layout->header.overflow_count++;
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOSPC;
    }

    /* Copy command to slot */
    memcpy(&ctx->layout->slots[producer], cmd, sizeof(struct shmem_cmd_slot));

    /* Update producer index */
    ctx->layout->header.producer_idx = next;
    ctx->layout->header.total_produced++;
    ctx->sent_count++;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

int shmem_consume_cmd(struct shmem_ctx *ctx, struct shmem_cmd_slot *cmd)
{
    if (!ctx || !cmd || !ctx->is_consumer) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctx->lock, 0);

    u32 producer = ctx->layout->header.producer_idx;
    u32 consumer = ctx->layout->header.consumer_idx;

    if (consumer == producer) {
        /* Empty */
        mutex_unlock(&ctx->lock);
        return HFSSS_ERR_NOENT;
    }

    /* Copy command from slot */
    memcpy(cmd, &ctx->layout->slots[consumer], sizeof(struct shmem_cmd_slot));

    /* Update consumer index */
    ctx->layout->header.consumer_idx = (consumer + 1) % RING_BUFFER_SLOTS;
    ctx->layout->header.total_consumed++;
    ctx->recv_count++;

    mutex_unlock(&ctx->lock);

    return HFSSS_OK;
}

void shmem_nvme_to_slot(struct shmem_cmd_slot *slot, const struct nvme_sq_entry *cmd, u16 qid)
{
    if (!slot || !cmd) {
        return;
    }

    memset(slot, 0, sizeof(*slot));

    slot->type = (qid == 0) ? SHMEM_CMD_NVME_ADMIN : SHMEM_CMD_NVME_IO;
    slot->timestamp = get_time_ns();
    slot->cmd_len = sizeof(struct nvme_sq_entry);

    memcpy(&slot->nvme_cmd, cmd, sizeof(struct nvme_sq_entry));
}

void shmem_slot_to_nvme(struct nvme_sq_entry *cmd, const struct shmem_cmd_slot *slot)
{
    if (!cmd || !slot) {
        return;
    }

    memcpy(cmd, &slot->nvme_cmd, sizeof(struct nvme_sq_entry));
}
