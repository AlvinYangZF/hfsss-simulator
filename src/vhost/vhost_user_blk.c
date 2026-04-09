/*
 * vhost_user_blk.c - vhost-user block device server
 *
 * Implements the vhost-user protocol (AF_UNIX SOCK_STREAM) so QEMU can use
 * the hfsss-simulator as a virtio-blk backend.
 *
 * Protocol reference: https://qemu.readthedocs.io/en/latest/interop/vhost-user.html
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "vhost/vhost_user_blk.h"
#include "vhost_user_proto.h"
#include "pcie/nvme_uspace.h"
#include "pcie/nvme.h"
#include "sssim.h"

#define LOG_MOD "vhost-blk"

/* Simple stderr logging — no log_ctx dependency in this module */
#define VB_INFO(fmt, ...) fprintf(stderr, "[INFO ][" LOG_MOD "] " fmt "\n", ##__VA_ARGS__)
#define VB_WARN(fmt, ...) fprintf(stderr, "[WARN ][" LOG_MOD "] " fmt "\n", ##__VA_ARGS__)
#define VB_ERROR(fmt, ...) fprintf(stderr, "[ERROR][" LOG_MOD "] " fmt "\n", ##__VA_ARGS__)

/* Maximum descriptor chain depth — safety limit */
#define VRING_CHAIN_MAX 256

/* Size of the SCM_RIGHTS control message buffer able to hold VHOST_USER_MAX_FDS */
#define CMSG_FDS_BUF_SIZE CMSG_SPACE(sizeof(int) * VHOST_USER_MAX_FDS)

/* -------------------------------------------------------------------------
 * Low-level socket helpers
 * ---------------------------------------------------------------------- */

/*
 * vhost_recv_msg - Receive one vhost-user message from fd.
 *
 * The fixed 12-byte header is read first, then up to msg->size bytes of
 * payload.  Any SCM_RIGHTS file descriptors are received into fds[].
 *
 * Returns 0 on success, -1 on error/EOF.
 */
static int vhost_recv_msg(int fd, struct vhost_user_msg *msg, int *fds, int *num_fds)
{
    struct iovec iov;
    struct msghdr mhdr;
    char cmsg_buf[CMSG_FDS_BUF_SIZE];
    ssize_t n;

    /* --- read header --------------------------------------------------- */
    iov.iov_base = msg;
    iov.iov_len = VHOST_USER_HDR_SIZE;

    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_iov = &iov;
    mhdr.msg_iovlen = 1;
    mhdr.msg_control = cmsg_buf;
    mhdr.msg_controllen = sizeof(cmsg_buf);

    n = recvmsg(fd, &mhdr, MSG_WAITALL);
    if (n <= 0) {
        if (n == 0) {
            VB_INFO("peer closed connection");
        } else {
            VB_ERROR("recvmsg header: %s", strerror(errno));
        }
        return -1;
    }
    if ((size_t)n < VHOST_USER_HDR_SIZE) {
        VB_ERROR("short header read: %zd bytes", n);
        return -1;
    }

    /* extract received file descriptors */
    *num_fds = 0;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mhdr);
    while (cmsg != NULL) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int nfds = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            int *rcv = (int *)CMSG_DATA(cmsg);
            int limit = nfds < VHOST_USER_MAX_FDS ? nfds : VHOST_USER_MAX_FDS;
            for (int i = 0; i < limit; i++) {
                fds[(*num_fds)++] = rcv[i];
            }
        }
        cmsg = CMSG_NXTHDR(&mhdr, cmsg);
    }

    /* --- read payload -------------------------------------------------- */
    if (msg->size == 0) {
        return 0;
    }
    if (msg->size > sizeof(msg->payload)) {
        VB_ERROR("payload too large: %u bytes", msg->size);
        return -1;
    }

    n = recv(fd, &msg->payload, msg->size, MSG_WAITALL);
    if (n <= 0 || (size_t)n < msg->size) {
        VB_ERROR("short payload read: %zd of %u bytes", n, msg->size);
        return -1;
    }

    return 0;
}

/*
 * vhost_send_reply - Send a reply message on fd.
 *
 * Sets VHOST_USER_REPLY_MASK in flags and transmits the header + payload.
 * Returns 0 on success, -1 on error.
 */
static int vhost_send_reply(int fd, struct vhost_user_msg *msg)
{
    msg->flags |= VHOST_USER_REPLY_MASK;

    /* Total wire size = header + payload */
    size_t total = VHOST_USER_HDR_SIZE + msg->size;
    ssize_t n = send(fd, msg, total, MSG_NOSIGNAL);
    if (n < 0 || (size_t)n < total) {
        VB_ERROR("send reply failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Guest-physical-address → host-virtual-address translation
 * ---------------------------------------------------------------------- */

/*
 * gpa_to_hva - Translate a guest physical address to a host virtual address.
 *
 * Walks the registered memory regions and returns a pointer into the
 * mmap'd region.  Returns NULL if gpa is not covered by any region.
 */
static void *gpa_to_hva(struct vhost_user_blk_server *srv, uint64_t gpa)
{
    for (int i = 0; i < srv->num_regions; i++) {
        const struct vhost_mem_region *r = &srv->mem_regions[i];
        if (gpa >= r->guest_phys && gpa < r->guest_phys + r->size) {
            /* mmap_addr is the base of the mmap; mmap_offset accounts for
             * the offset within the file/shmem that was mapped. */
            uintptr_t base = (uintptr_t)r->mmap_addr + r->mmap_offset;
            return (void *)(base + (gpa - r->guest_phys));
        }
    }
    VB_ERROR("gpa_to_hva: unmapped GPA 0x%016llx", (unsigned long long)gpa);
    return NULL;
}

/*
 * user_addr_to_hva - Translate a QEMU userspace address to host virtual.
 *
 * SET_VRING_ADDR carries QEMU process virtual addresses, not guest physical.
 * This translates through userspace_addr ranges from SET_MEM_TABLE.
 */
static void *user_addr_to_hva(struct vhost_user_blk_server *srv, uint64_t uaddr)
{
    for (int i = 0; i < srv->num_regions; i++) {
        const struct vhost_mem_region *r = &srv->mem_regions[i];
        if (uaddr >= r->userspace_addr && uaddr < r->userspace_addr + r->size) {
            uintptr_t base = (uintptr_t)r->mmap_addr + r->mmap_offset;
            return (void *)(base + (uaddr - r->userspace_addr));
        }
    }
    VB_ERROR("user_addr_to_hva: unmapped addr 0x%016llx", (unsigned long long)uaddr);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Block I/O request processing
 * ---------------------------------------------------------------------- */

/*
 * process_blk_req - Dispatch a parsed virtio-blk request to the NVMe device.
 *
 * Returns VIRTIO_BLK_S_OK on success, VIRTIO_BLK_S_IOERR on failure,
 * VIRTIO_BLK_S_UNSUPP for unsupported request types.
 */
static uint8_t process_blk_req(struct vhost_user_blk_server *srv, const struct virtio_blk_outhdr *hdr, void *data_buf,
                               uint32_t data_len)
{
    struct nvme_uspace_dev *dev = srv->dev;
    uint32_t blk_size = srv->blk_size;
    int rc;

    switch (hdr->type) {
    case VIRTIO_BLK_T_IN: {
        /* sector → LBA conversion (sectors are 512-byte units) */
        uint64_t lba = hdr->sector * 512 / blk_size;
        uint32_t count = data_len / blk_size;
        if (count == 0) {
            VB_WARN("T_IN: zero-count read (data_len=%u blk_size=%u)", data_len, blk_size);
            return VIRTIO_BLK_S_IOERR;
        }
        rc = nvme_uspace_read(dev, 1, lba, count, data_buf);
        if (rc != 0) {
            VB_ERROR("nvme_uspace_read lba=%llu count=%u rc=%d", (unsigned long long)lba, count, rc);
            return VIRTIO_BLK_S_IOERR;
        }
        return VIRTIO_BLK_S_OK;
    }

    case VIRTIO_BLK_T_OUT: {
        uint64_t lba = hdr->sector * 512 / blk_size;
        uint32_t count = data_len / blk_size;
        if (count == 0) {
            VB_WARN("T_OUT: zero-count write (data_len=%u blk_size=%u)", data_len, blk_size);
            return VIRTIO_BLK_S_IOERR;
        }
        rc = nvme_uspace_write(dev, 1, lba, count, data_buf);
        if (rc != 0) {
            VB_ERROR("nvme_uspace_write lba=%llu count=%u rc=%d", (unsigned long long)lba, count, rc);
            return VIRTIO_BLK_S_IOERR;
        }
        return VIRTIO_BLK_S_OK;
    }

    case VIRTIO_BLK_T_FLUSH:
        rc = nvme_uspace_flush(dev, 1);
        if (rc != 0) {
            VB_ERROR("nvme_uspace_flush rc=%d", rc);
            return VIRTIO_BLK_S_IOERR;
        }
        return VIRTIO_BLK_S_OK;

    case VIRTIO_BLK_T_DISCARD: {
        /*
         * The data buffer contains an array of virtio_blk_discard_write_zeroes
         * entries.  Each entry maps to one NVMe DSM range.
         * virtio-blk discard entry layout (per spec §5.2.6.4):
         *   u64 sector
         *   u32 num_sectors   (in 512-byte units)
         *   u32 flags
         */
        typedef struct {
            uint64_t sector;
            uint32_t num_sectors;
            uint32_t flags;
        } vblk_discard_t;

        uint32_t nr = data_len / (uint32_t)sizeof(vblk_discard_t);
        if (nr == 0) {
            return VIRTIO_BLK_S_IOERR;
        }

        /* Allocate DSM ranges on the stack for small counts, heap otherwise */
        struct nvme_dsm_range *ranges = NULL;
        bool heap = (nr > 32);
        struct nvme_dsm_range stack_ranges[32];

        if (heap) {
            ranges = (struct nvme_dsm_range *)malloc(nr * sizeof(struct nvme_dsm_range));
            if (!ranges) {
                VB_ERROR("discard: OOM for %u ranges", nr);
                return VIRTIO_BLK_S_IOERR;
            }
        } else {
            ranges = stack_ranges;
        }

        const vblk_discard_t *entries = (const vblk_discard_t *)data_buf;
        for (uint32_t i = 0; i < nr; i++) {
            ranges[i].attributes = 0;
            ranges[i].slba = entries[i].sector * 512 / blk_size;
            ranges[i].nlb = entries[i].num_sectors * 512 / blk_size;
        }

        rc = nvme_uspace_trim(dev, 1, ranges, nr);

        if (heap) {
            free(ranges);
        }

        if (rc != 0) {
            VB_ERROR("nvme_uspace_trim nr=%u rc=%d", nr, rc);
            return VIRTIO_BLK_S_IOERR;
        }
        return VIRTIO_BLK_S_OK;
    }

    default:
        VB_WARN("unsupported request type %u", hdr->type);
        return VIRTIO_BLK_S_UNSUPP;
    }
}

/* -------------------------------------------------------------------------
 * Virtqueue processing
 * ---------------------------------------------------------------------- */

/*
 * process_vring - Drain all pending requests from virtqueue vq_idx.
 *
 * For each available descriptor chain:
 *   1. Parse the three logical segments: header, data buffer(s), status byte.
 *   2. Dispatch to process_blk_req().
 *   3. Write back the status byte and update the used ring.
 *   4. Signal the guest via the call eventfd.
 */
static int process_vring(struct vhost_user_blk_server *srv, int vq_idx)
{
    if (vq_idx < 0 || vq_idx >= VHOST_MAX_VRINGS) {
        return -1;
    }

    struct vhost_vring *vq = &srv->vrings[vq_idx];

    if (!vq->enabled || !vq->desc_addr || !vq->avail_addr || !vq->used_addr) {
        return 0;
    }

    struct vring_avail *avail = (struct vring_avail *)vq->avail_addr;
    struct vring_used *used = (struct vring_used *)vq->used_addr;
    struct vring_desc *descs = (struct vring_desc *)vq->desc_addr;

    /* Acquire fence before reading avail->idx written by the guest */
    __sync_synchronize();

    uint16_t avail_idx = avail->idx;

    while (vq->last_avail_idx != avail_idx) {
        uint16_t ring_slot = vq->last_avail_idx % vq->num;
        uint16_t head_idx = avail->ring[ring_slot];
        vq->last_avail_idx++;

        if (head_idx >= vq->num) {
            VB_ERROR("vring[%d]: invalid head descriptor index %u", vq_idx, head_idx);
            continue;
        }

        /* --- walk descriptor chain ------------------------------------ */
        struct virtio_blk_outhdr hdr;
        bool hdr_filled = false;
        uint8_t *status_ptr = NULL;
        uint32_t total_written = 0;

        /* Scatter-gather: collect data segments into a linear buffer */
        uint8_t *sg_buf = NULL;
        uint32_t sg_len = 0;
        uint32_t sg_cap = 0;

        uint16_t desc_idx = head_idx;
        int depth = 0;

        while (depth < VRING_CHAIN_MAX) {
            struct vring_desc *d = &descs[desc_idx];

            void *hva = gpa_to_hva(srv, d->addr);
            if (!hva) {
                VB_ERROR("vring[%d]: GPA translation failed for desc %u", vq_idx, desc_idx);
                break;
            }

            bool is_write = (d->flags & VRING_DESC_F_WRITE) != 0;
            bool has_next = (d->flags & VRING_DESC_F_NEXT) != 0;

            if (!hdr_filled) {
                /* First descriptor — device-readable request header */
                if (d->len < sizeof(hdr)) {
                    VB_ERROR("vring[%d]: hdr desc too short (%u bytes)", vq_idx, d->len);
                    break;
                }
                memcpy(&hdr, hva, sizeof(hdr));
                hdr_filled = true;
            } else if (!has_next) {
                /* Last descriptor — device-writable status byte */
                if (!is_write || d->len < 1) {
                    VB_ERROR("vring[%d]: bad status desc (write=%d len=%u)", vq_idx, (int)is_write, d->len);
                }
                status_ptr = (uint8_t *)hva;
            } else {
                /* Middle descriptors — data payload (may be multi-segment) */
                uint32_t new_len = sg_len + d->len;
                if (new_len > sg_cap) {
                    sg_cap = (new_len < 4096) ? 4096 : new_len;
                    sg_buf = realloc(sg_buf, sg_cap);
                }
                if (is_write) {
                    /* Device-writable: read target — will be filled later */
                    total_written += d->len;
                } else {
                    /* Device-readable: write data from guest */
                    memcpy(sg_buf + sg_len, hva, d->len);
                }
                sg_len = new_len;
            }

            if (!has_next) {
                break;
            }

            /* Bounds-check next descriptor index */
            if (d->next >= vq->num) {
                VB_ERROR("vring[%d]: desc.next %u out of range (num=%u)", vq_idx, d->next, vq->num);
                break;
            }
            desc_idx = d->next;
            depth++;
        }

        /* --- dispatch request ----------------------------------------- */
        uint8_t status = VIRTIO_BLK_S_IOERR;

        if (!hdr_filled) {
            VB_ERROR("vring[%d]: no header in descriptor chain", vq_idx);
        } else if (!status_ptr) {
            VB_ERROR("vring[%d]: no status descriptor in chain", vq_idx);
        } else {
            status = process_blk_req(srv, &hdr, sg_buf, sg_len);
        }

        /* For read requests, scatter the linear buffer back to guest segments */
        if (status == VIRTIO_BLK_S_OK && sg_buf && total_written > 0) {
            uint16_t si = head_idx;
            uint32_t off = 0;
            int sd = 0;
            while (sd < VRING_CHAIN_MAX && off < sg_len) {
                struct vring_desc *dd = &descs[si];
                bool dw = (dd->flags & VRING_DESC_F_WRITE) != 0;
                bool dn = (dd->flags & VRING_DESC_F_NEXT) != 0;
                if (dw && dn) {
                    void *dst = gpa_to_hva(srv, dd->addr);
                    if (dst) {
                        uint32_t copy_len = (dd->len < sg_len - off) ? dd->len : sg_len - off;
                        memcpy(dst, sg_buf + off, copy_len);
                        off += copy_len;
                    }
                }
                if (!dn)
                    break;
                if (dd->next >= vq->num)
                    break;
                si = dd->next;
                sd++;
            }
        }
        free(sg_buf);

        /* --- write status byte ---------------------------------------- */
        if (status_ptr) {
            *status_ptr = status;
        }

        /* --- update used ring ----------------------------------------- */
        uint16_t used_slot = used->idx % vq->num;
        used->ring[used_slot].id = head_idx;
        used->ring[used_slot].len = total_written;

        /* Release fence: ensure used ring entry is visible before idx update */
        __sync_synchronize();

        used->idx++;

        /* --- notify guest --------------------------------------------- */
        if (vq->call_fd >= 0) {
            uint64_t val = 1;
            ssize_t n = write(vq->call_fd, &val, sizeof(val));
            if (n != sizeof(val)) {
                VB_WARN("vring[%d]: call_fd write failed: %s", vq_idx, strerror(errno));
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * vhost-user message handling
 * ---------------------------------------------------------------------- */

/*
 * handle_msg - Process one vhost-user control message.
 *
 * Returns  0 on success (continue running),
 *         -1 on fatal error (close connection).
 */
static int handle_msg(struct vhost_user_blk_server *srv, struct vhost_user_msg *msg, int *fds, int num_fds)
{
    switch (msg->request) {

    /* ------------------------------------------------------------------ */
    case VHOST_USER_GET_FEATURES: {
        uint64_t features = (1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                            (1ULL << VIRTIO_BLK_F_FLUSH) | (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                            (1ULL << VIRTIO_BLK_F_DISCARD) | (1ULL << VIRTIO_F_VERSION_1) |
                            (1ULL << VHOST_USER_F_PROTOCOL_FEATURES);

        msg->payload.u64 = features;
        msg->size = sizeof(uint64_t);
        return vhost_send_reply(srv->conn_fd, msg);
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_FEATURES:
        srv->features = msg->payload.u64;
        VB_INFO("SET_FEATURES: 0x%016llx", (unsigned long long)srv->features);
        return 0;

    /* ------------------------------------------------------------------ */
    case VHOST_USER_GET_PROTOCOL_FEATURES: {
        uint64_t pf = (1ULL << VHOST_USER_PROTOCOL_F_CONFIG);
        msg->payload.u64 = pf;
        msg->size = sizeof(uint64_t);
        return vhost_send_reply(srv->conn_fd, msg);
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_PROTOCOL_FEATURES:
        srv->protocol_features = msg->payload.u64;
        VB_INFO("SET_PROTOCOL_FEATURES: 0x%016llx", (unsigned long long)srv->protocol_features);
        return 0;

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_MEM_TABLE: {
        uint32_t nr = msg->payload.mem_table.num_regions;
        if (nr > VHOST_MAX_MEM_REGIONS) {
            VB_ERROR("SET_MEM_TABLE: too many regions %u", nr);
            return -1;
        }
        if (num_fds < (int)nr) {
            VB_ERROR("SET_MEM_TABLE: expected %u fds, got %d", nr, num_fds);
            return -1;
        }

        /* Unmap any previously registered regions */
        for (int i = 0; i < srv->num_regions; i++) {
            struct vhost_mem_region *r = &srv->mem_regions[i];
            if (r->mmap_addr) {
                munmap(r->mmap_addr, r->mmap_len);
                r->mmap_addr = NULL;
            }
            if (r->fd >= 0) {
                close(r->fd);
                r->fd = -1;
            }
        }

        for (uint32_t i = 0; i < nr; i++) {
            const struct vhost_user_mem_region *src = &msg->payload.mem_table.regions[i];
            struct vhost_mem_region *dst = &srv->mem_regions[i];

            dst->guest_phys = src->guest_phys_addr;
            dst->userspace_addr = src->userspace_addr;
            dst->size = src->memory_size;
            dst->mmap_offset = src->mmap_offset;
            dst->fd = fds[i];
            dst->mmap_len = src->memory_size + src->mmap_offset;

            dst->mmap_addr =
                mmap(NULL, dst->mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, dst->fd, 0 /* map from file start */);
            if (dst->mmap_addr == MAP_FAILED) {
                VB_ERROR("SET_MEM_TABLE: mmap region %u failed: %s", i, strerror(errno));
                dst->mmap_addr = NULL;
                return -1;
            }

            VB_INFO("region[%u]: gpa=0x%llx size=0x%llx mmap_offset=0x%llx", i, (unsigned long long)dst->guest_phys,
                    (unsigned long long)dst->size, (unsigned long long)dst->mmap_offset);
        }
        srv->num_regions = (int)nr;
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_VRING_NUM: {
        uint32_t idx = msg->payload.vring_state.index;
        uint32_t num = msg->payload.vring_state.num;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("SET_VRING_NUM: index %u out of range", idx);
            return 0;
        }
        srv->vrings[idx].num = num;
        VB_INFO("vring[%u] num=%u", idx, num);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_VRING_ADDR: {
        uint32_t idx = msg->payload.vring_addr.index;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("SET_VRING_ADDR: index %u out of range", idx);
            return 0;
        }
        struct vhost_vring *vq = &srv->vrings[idx];
        vq->desc_addr = user_addr_to_hva(srv, msg->payload.vring_addr.desc_addr);
        vq->avail_addr = user_addr_to_hva(srv, msg->payload.vring_addr.avail_addr);
        vq->used_addr = user_addr_to_hva(srv, msg->payload.vring_addr.used_addr);

        if (!vq->desc_addr || !vq->avail_addr || !vq->used_addr) {
            VB_ERROR("SET_VRING_ADDR[%u]: address translation failed", idx);
            return -1;
        }
        VB_INFO("vring[%u] desc=%p avail=%p used=%p", idx, vq->desc_addr, vq->avail_addr, vq->used_addr);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_VRING_BASE: {
        uint32_t idx = msg->payload.vring_state.index;
        uint32_t base = msg->payload.vring_state.num;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("SET_VRING_BASE: index %u out of range", idx);
            return 0;
        }
        srv->vrings[idx].last_avail_idx = (uint16_t)base;
        VB_INFO("vring[%u] last_avail_idx=%u", idx, base);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_GET_VRING_BASE: {
        uint32_t idx = msg->payload.vring_state.index;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("GET_VRING_BASE: index %u out of range", idx);
            idx = 0;
        }
        srv->vrings[idx].enabled = false;

        msg->payload.vring_state.index = idx;
        msg->payload.vring_state.num = srv->vrings[idx].last_avail_idx;
        msg->size = sizeof(msg->payload.vring_state);
        return vhost_send_reply(srv->conn_fd, msg);
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_VRING_KICK: {
        uint32_t idx = msg->payload.u64 & 0xFF;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("SET_VRING_KICK: index %u out of range", idx);
            /* close received fd to avoid leak */
            for (int i = 0; i < num_fds; i++) {
                close(fds[i]);
            }
            return 0;
        }
        /* Close old kick fd if present */
        if (srv->vrings[idx].kick_fd >= 0) {
            close(srv->vrings[idx].kick_fd);
        }
        srv->vrings[idx].kick_fd = (num_fds > 0) ? fds[0] : -1;
        VB_INFO("vring[%u] kick_fd=%d", idx, srv->vrings[idx].kick_fd);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_VRING_CALL: {
        uint32_t idx = msg->payload.u64 & 0xFF;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("SET_VRING_CALL: index %u out of range", idx);
            for (int i = 0; i < num_fds; i++) {
                close(fds[i]);
            }
            return 0;
        }
        if (srv->vrings[idx].call_fd >= 0) {
            close(srv->vrings[idx].call_fd);
        }
        srv->vrings[idx].call_fd = (num_fds > 0) ? fds[0] : -1;
        VB_INFO("vring[%u] call_fd=%d", idx, srv->vrings[idx].call_fd);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_VRING_ENABLE: {
        uint32_t idx = msg->payload.vring_state.index;
        uint32_t enable = msg->payload.vring_state.num;
        if (idx >= VHOST_MAX_VRINGS) {
            VB_WARN("SET_VRING_ENABLE: index %u out of range", idx);
            return 0;
        }
        srv->vrings[idx].enabled = (enable != 0);
        VB_INFO("vring[%u] enabled=%d", idx, (int)srv->vrings[idx].enabled);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_GET_CONFIG: {
        struct virtio_blk_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.capacity = srv->capacity_sectors;
        cfg.blk_size = srv->blk_size;
        cfg.size_max = 1u << 20; /* 1 MiB */
        cfg.seg_max = 128;

        /* Copy into the config payload region */
        uint32_t copy_len = msg->payload.config.size;
        if (copy_len > sizeof(cfg)) {
            copy_len = sizeof(cfg);
        }

        memcpy(msg->payload.config.region, &cfg, copy_len);
        msg->payload.config.size = copy_len;
        msg->payload.config.flags = 0;
        msg->size = (uint32_t)(offsetof(typeof(msg->payload.config), region) + copy_len);
        return vhost_send_reply(srv->conn_fd, msg);
    }

    /* ------------------------------------------------------------------ */
    case VHOST_USER_SET_OWNER:
        /* No per-connection state needed */
        return 0;

    /* ------------------------------------------------------------------ */
    case VHOST_USER_RESET_OWNER:
        return 0;

    /* ------------------------------------------------------------------ */
    default:
        VB_WARN("unhandled message type %u — ignoring", msg->request);
        /* If the request needs a reply (bit 3 set in flags), send a dummy */
        return 0;
    }
}

/* -------------------------------------------------------------------------
 * Server initialisation / shutdown
 * ---------------------------------------------------------------------- */

/*
 * vhost_blk_server_init - Create the UNIX domain socket and prepare the server.
 *
 * Calculates device capacity from the simulator config, creates an AF_UNIX
 * SOCK_STREAM socket, binds to socket_path, and calls listen(1).
 *
 * Returns 0 on success, -1 on error.
 */
int vhost_blk_server_init(struct vhost_user_blk_server *srv, const char *socket_path, struct nvme_uspace_dev *dev)
{
    if (!srv || !socket_path || !dev) {
        return -1;
    }

    memset(srv, 0, sizeof(*srv));
    srv->dev = dev;
    srv->listen_fd = -1;
    srv->conn_fd = -1;

    /* Initialise vring fd fields to -1 (invalid) */
    for (int i = 0; i < VHOST_MAX_VRINGS; i++) {
        srv->vrings[i].kick_fd = -1;
        srv->vrings[i].call_fd = -1;
    }
    for (int i = 0; i < VHOST_MAX_MEM_REGIONS; i++) {
        srv->mem_regions[i].fd = -1;
    }

    /* Derive capacity from simulator config */
    const struct sssim_config *cfg = &dev->sssim.config;
    srv->blk_size = cfg->lba_size;
    srv->capacity_sectors = cfg->total_lbas * cfg->lba_size / 512;

    VB_INFO("capacity=%llu sectors, blk_size=%u", (unsigned long long)srv->capacity_sectors, srv->blk_size);

    /* Save socket path for cleanup */
    strncpy(srv->socket_path, socket_path, sizeof(srv->socket_path) - 1);
    srv->socket_path[sizeof(srv->socket_path) - 1] = '\0';

    /* Create the UNIX socket */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        VB_ERROR("socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    /* Remove stale socket file if present */
    unlink(socket_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        VB_ERROR("bind(%s): %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        VB_ERROR("listen: %s", strerror(errno));
        close(fd);
        unlink(socket_path);
        return -1;
    }

    srv->listen_fd = fd;
    srv->running = true;

    VB_INFO("listening on %s", socket_path);
    return 0;
}

/*
 * vhost_blk_server_stop - Signal the server to stop.
 */
void vhost_blk_server_stop(struct vhost_user_blk_server *srv)
{
    if (srv) {
        srv->running = false;
    }
}

/*
 * vhost_blk_server_cleanup - Release all resources held by the server.
 */
void vhost_blk_server_cleanup(struct vhost_user_blk_server *srv)
{
    if (!srv) {
        return;
    }

    /* Close connection socket */
    if (srv->conn_fd >= 0) {
        close(srv->conn_fd);
        srv->conn_fd = -1;
    }

    /* Close listen socket */
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }

    /* Unlink socket file */
    if (srv->socket_path[0] != '\0') {
        unlink(srv->socket_path);
    }

    /* Unmap memory regions and close their fds */
    for (int i = 0; i < srv->num_regions; i++) {
        struct vhost_mem_region *r = &srv->mem_regions[i];
        if (r->mmap_addr) {
            munmap(r->mmap_addr, r->mmap_len);
            r->mmap_addr = NULL;
        }
        if (r->fd >= 0) {
            close(r->fd);
            r->fd = -1;
        }
    }
    srv->num_regions = 0;

    /* Close vring fds */
    for (int i = 0; i < VHOST_MAX_VRINGS; i++) {
        if (srv->vrings[i].kick_fd >= 0) {
            close(srv->vrings[i].kick_fd);
            srv->vrings[i].kick_fd = -1;
        }
        if (srv->vrings[i].call_fd >= 0) {
            close(srv->vrings[i].call_fd);
            srv->vrings[i].call_fd = -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * Main server loop
 * ---------------------------------------------------------------------- */

/*
 * vhost_blk_server_run - Accept one QEMU connection and serve it.
 *
 * Phase 1: Receive and process control messages until all vrings are
 *          configured and enabled.
 * Phase 2: Poll on conn_fd (more control messages) and kick fds (I/O
 *          requests) simultaneously, dispatching as they arrive.
 *
 * Returns 0 on clean exit, -1 on fatal error.
 */
int vhost_blk_server_run(struct vhost_user_blk_server *srv)
{
    if (!srv || srv->listen_fd < 0) {
        return -1;
    }

    /* Accept exactly one connection from QEMU */
    VB_INFO("waiting for QEMU connection...");
    srv->conn_fd = accept(srv->listen_fd, NULL, NULL);
    if (srv->conn_fd < 0) {
        VB_ERROR("accept: %s", strerror(errno));
        return -1;
    }
    VB_INFO("QEMU connected (fd=%d)", srv->conn_fd);

    /*
     * Build poll array:
     *   [0]        : conn_fd  — control messages
     *   [1..2]     : kick_fd per vring
     */
    struct pollfd pfds[1 + VHOST_MAX_VRINGS];

    while (srv->running) {
        int nfds = 0;

        /* conn_fd always polled */
        pfds[nfds].fd = srv->conn_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;

        /* kick fds for enabled vrings */
        for (int i = 0; i < VHOST_MAX_VRINGS; i++) {
            if (srv->vrings[i].enabled && srv->vrings[i].kick_fd >= 0) {
                pfds[nfds].fd = srv->vrings[i].kick_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                /* store vring index in the user_data slot (reuse via padding) */
                /* We identify the vring by matching fd below */
                nfds++;
            }
        }

        int ret = poll(pfds, (nfds_t)nfds, -1 /* block indefinitely */);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            VB_ERROR("poll: %s", strerror(errno));
            break;
        }

        /* --- handle conn_fd events ------------------------------------ */
        if (pfds[0].revents & (POLLERR | POLLHUP)) {
            VB_INFO("connection closed by peer");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            struct vhost_user_msg msg;
            int fds[VHOST_USER_MAX_FDS];
            int num_fds = 0;

            memset(&msg, 0, sizeof(msg));
            if (vhost_recv_msg(srv->conn_fd, &msg, fds, &num_fds) < 0) {
                VB_INFO("connection lost while receiving message");
                break;
            }

            if (handle_msg(srv, &msg, fds, num_fds) < 0) {
                VB_ERROR("fatal error handling message type %u", msg.request);
                break;
            }
        }

        /* --- handle kick fd events ------------------------------------ */
        for (int pi = 1; pi < nfds; pi++) {
            if (!(pfds[pi].revents & POLLIN)) {
                continue;
            }

            /* Drain the eventfd counter (Linux eventfd semantics) */
            uint64_t kick_val = 0;
            ssize_t n = read(pfds[pi].fd, &kick_val, sizeof(kick_val));
            (void)n; /* best-effort; macOS uses plain write(1) too */

            /* Find which vring owns this kick fd */
            for (int vi = 0; vi < VHOST_MAX_VRINGS; vi++) {
                if (srv->vrings[vi].kick_fd == pfds[pi].fd) {
                    (void)process_vring(srv, vi);
                    break;
                }
            }
        }
    }

    close(srv->conn_fd);
    srv->conn_fd = -1;

    return 0;
}
