#ifndef HFSSS_NVME_KMOD_H
#define HFSSS_NVME_KMOD_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/blk-mq.h>
#include <linux/nvme.h>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

/* Vendor/Device IDs for HFSSS virtual NVMe device */
#define HFSSS_PCI_VENDOR_ID     0x1DEA
#define HFSSS_PCI_DEVICE_ID     0x4E56  /* 'NV' */
#define HFSSS_PCI_SUBSYS_VENDOR 0x1DEA
#define HFSSS_PCI_SUBSYS_DEVICE 0x0001

/* BAR0 size: 16KB (NVMe register space) */
#define HFSSS_BAR0_SIZE         (16 * 1024)

/* NVMe CAP register default value:
 *   MQES=4095 (max queue entries), CQR=1, AMS=0, TO=40 (500ms),
 *   DSTRD=0, NSSRS=0, CSS=NVM, BPS=0, MPSMIN=0, MPSMAX=0 */
#define HFSSS_NVME_CAP_DEFAULT  0x0020002800000FFFLLU

/* Shared memory ring buffer between kernel module and user-space simulator */
#define HFSSS_SHMEM_MAGIC       0x48465353u  /* 'HFSS' */
#define HFSSS_SHMEM_VERSION     1
#define HFSSS_SHMEM_RING_SIZE   1024         /* command ring entries */

#ifndef __KERNEL__
/* User-space view of the shared memory header */
struct hfsss_shmem_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t ring_size;
    uint32_t _pad;
    uint64_t submit_head;   /* kernel writes here */
    uint64_t submit_tail;   /* user-space reads here */
    uint64_t complete_head; /* user-space writes here */
    uint64_t complete_tail; /* kernel reads here */
};
#endif /* !__KERNEL__ */

#endif /* HFSSS_NVME_KMOD_H */
