#ifndef __HFSSS_VHOST_USER_BLK_H
#define __HFSSS_VHOST_USER_BLK_H

#include <stdint.h>
#include <stdbool.h>
#include <poll.h>

/* Forward declaration */
struct nvme_uspace_dev;

#define VHOST_MAX_VRINGS 2
#define VHOST_MAX_MEM_REGIONS 8

struct vhost_mem_region {
    uint64_t guest_phys;
    uint64_t userspace_addr;
    uint64_t size;
    uint64_t mmap_offset;
    void *mmap_addr;
    size_t mmap_len;
    int fd;
};

struct vhost_vring {
    int kick_fd;
    int call_fd;
    uint32_t num;    /* queue size */
    void *desc_addr; /* host-virtual pointer to descriptor table */
    void *avail_addr;
    void *used_addr;
    uint16_t last_avail_idx;
    bool enabled;
};

struct vhost_user_blk_server {
    /* Socket */
    int listen_fd;
    int conn_fd;

    /* Simulator device */
    struct nvme_uspace_dev *dev;

    /* vhost-user negotiated state */
    uint64_t features;
    uint64_t protocol_features;

    /* Memory regions from QEMU */
    struct vhost_mem_region mem_regions[VHOST_MAX_MEM_REGIONS];
    int num_regions;

    /* Virtqueues */
    struct vhost_vring vrings[VHOST_MAX_VRINGS];
    int num_vrings;

    /* Device config */
    uint64_t capacity_sectors; /* in 512-byte sectors */
    uint32_t blk_size;

    /* Socket path saved for cleanup */
    char socket_path[256];

    /* State */
    bool running;
};

/* API */
int vhost_blk_server_init(struct vhost_user_blk_server *srv, const char *socket_path, struct nvme_uspace_dev *dev);
int vhost_blk_server_run(struct vhost_user_blk_server *srv);
void vhost_blk_server_stop(struct vhost_user_blk_server *srv);
void vhost_blk_server_cleanup(struct vhost_user_blk_server *srv);

#endif /* __HFSSS_VHOST_USER_BLK_H */
