#include "controller/shmem_if.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int shmem_if_open(const char *path, struct shmem_layout **shmem_out, int *fd_out)
{
    int fd;
    struct shmem_layout *shmem;

    if (!path || !shmem_out || !fd_out) {
        return HFSSS_ERR_INVAL;
    }

    /* Try to open existing shared memory */
    fd = shm_open(path, O_RDWR, 0666);
    if (fd < 0) {
        return HFSSS_ERR_NOTSUPP;
    }

    /* Map shared memory */
    shmem = (struct shmem_layout *)mmap(NULL, sizeof(struct shmem_layout),
                                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmem == MAP_FAILED) {
        close(fd);
        return HFSSS_ERR_NOMEM;
    }

    *shmem_out = shmem;
    *fd_out = fd;

    return HFSSS_OK;
}

void shmem_if_close(struct shmem_layout *shmem, int fd)
{
    if (!shmem || fd < 0) {
        return;
    }

    munmap(shmem, sizeof(struct shmem_layout));
    close(fd);
}

int shmem_if_receive_cmd(struct shmem_layout *shmem, struct nvme_cmd_from_kern *cmd)
{
    struct ring_slot *slot;
    u32 cons_idx;

    if (!shmem || !cmd) {
        return HFSSS_ERR_INVAL;
    }

    cons_idx = shmem->header.cons_idx;
    if (cons_idx == shmem->header.prod_idx) {
        return HFSSS_ERR_AGAIN;
    }

    slot = &shmem->slots[cons_idx % RING_BUFFER_SLOTS];

    if (!slot->ready) {
        return HFSSS_ERR_AGAIN;
    }

    memcpy(cmd, &slot->cmd, sizeof(*cmd));
    slot->ready = 0;

    shmem->header.cons_idx = (cons_idx + 1) % RING_BUFFER_SLOTS;
    shmem->header.cons_seq++;

    return HFSSS_OK;
}

int shmem_if_send_cpl(struct shmem_layout *shmem, struct nvme_cpl_to_kern *cpl)
{
    struct ring_slot *slot;
    u32 prod_idx;

    if (!shmem || !cpl) {
        return HFSSS_ERR_INVAL;
    }

    prod_idx = shmem->header.prod_idx;

    slot = &shmem->slots[prod_idx % RING_BUFFER_SLOTS];

    if (slot->done) {
        return HFSSS_ERR_AGAIN;
    }

    slot->ready = 0;
    slot->done = 1;

    shmem->header.prod_idx = (prod_idx + 1) % RING_BUFFER_SLOTS;
    shmem->header.prod_seq++;

    return HFSSS_OK;
}
