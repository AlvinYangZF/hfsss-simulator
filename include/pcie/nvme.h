#ifndef __HFSSS_NVME_H
#define __HFSSS_NVME_H

#include "common/common.h"
#include "pcie/pci.h"

/* NVMe Version */
#define NVME_VERSION_1_0 0x00010000
#define NVME_VERSION_1_1 0x00010100
#define NVME_VERSION_1_2 0x00010200
#define NVME_VERSION_1_3 0x00010300
#define NVME_VERSION_1_4 0x00010400
#define NVME_VERSION_2_0 0x00020000

/* NVMe Controller Register Offsets */
#define NVME_REG_CAP      0x00  /* Controller Capabilities (64-bit) */
#define NVME_REG_VS       0x08  /* Version (32-bit) */
#define NVME_REG_INTMS    0x0C  /* Interrupt Mask Set (32-bit) */
#define NVME_REG_INTMC    0x10  /* Interrupt Mask Clear (32-bit) */
#define NVME_REG_CC       0x14  /* Controller Configuration (32-bit) */
#define NVME_REG_CSTS     0x1C  /* Controller Status (32-bit) */
#define NVME_REG_NSSR     0x20  /* NVM Subsystem Reset (32-bit) */
#define NVME_REG_AQA      0x24  /* Admin Queue Attributes (32-bit) */
#define NVME_REG_ASQ      0x28  /* Admin Submission Queue Base (64-bit) */
#define NVME_REG_ACQ      0x30  /* Admin Completion Queue Base (64-bit) */
#define NVME_REG_CMBLOC   0x38  /* Controller Memory Buffer Location (32-bit) */
#define NVME_REG_CMBSZ    0x3C  /* Controller Memory Buffer Size (32-bit) */
#define NVME_REG_BPINFO   0x40  /* Boot Partition Information (32-bit) */
#define NVME_REG_BPRSEL   0x44  /* Boot Partition Read Select (32-bit) */
#define NVME_REG_BPMBL    0x48  /* Boot Partition Memory Buffer Location (64-bit) */
#define NVME_REG_DBS      0x1000 /* Doorbell Registers Start */

/* CAP Register Bit Definitions */
#define NVME_CAP_MQES_SHIFT   0
#define NVME_CAP_MQES_MASK    (0xFFFFULL << NVME_CAP_MQES_SHIFT)
#define NVME_CAP_CQR_SHIFT    16
#define NVME_CAP_CQR_MASK     (0x1ULL << NVME_CAP_CQR_SHIFT)
#define NVME_CAP_AMS_SHIFT    17
#define NVME_CAP_AMS_MASK     (0x7ULL << NVME_CAP_AMS_SHIFT)
#define NVME_CAP_TO_SHIFT     24
#define NVME_CAP_TO_MASK      (0xFFULL << NVME_CAP_TO_SHIFT)
#define NVME_CAP_DSTRD_SHIFT  32
#define NVME_CAP_DSTRD_MASK   (0xFULL << NVME_CAP_DSTRD_SHIFT)
#define NVME_CAP_NSSRS_SHIFT  36
#define NVME_CAP_NSSRS_MASK   (0x1ULL << NVME_CAP_NSSRS_SHIFT)
#define NVME_CAP_CSS_SHIFT    37
#define NVME_CAP_CSS_MASK     (0xFFULL << NVME_CAP_CSS_SHIFT)
#define NVME_CAP_MPSMIN_SHIFT 48
#define NVME_CAP_MPSMIN_MASK  (0xFULL << NVME_CAP_MPSMIN_SHIFT)
#define NVME_CAP_MPSMAX_SHIFT 52
#define NVME_CAP_MPSMAX_MASK  (0xFULL << NVME_CAP_MPSMAX_SHIFT)

/* AMS Values */
#define NVME_AMS_RR           0  /* Round Robin */
#define NVME_AMS_WRR          1  /* Weighted Round Robin */
#define NVME_AMS_VENDOR       7  /* Vendor Specific */

/* CSS Values */
#define NVME_CSS_NVM          0x01  /* NVM Command Set */

/* CC Register Bit Definitions */
#define NVME_CC_EN_SHIFT      0
#define NVME_CC_EN_MASK       (0x1U << NVME_CC_EN_SHIFT)
#define NVME_CC_CSS_SHIFT     4
#define NVME_CC_CSS_MASK      (0x7U << NVME_CC_CSS_SHIFT)
#define NVME_CC_MPS_SHIFT     7
#define NVME_CC_MPS_MASK      (0xFU << NVME_CC_MPS_SHIFT)
#define NVME_CC_AMS_SHIFT     11
#define NVME_CC_AMS_MASK      (0x7U << NVME_CC_AMS_SHIFT)
#define NVME_CC_SHN_SHIFT     14
#define NVME_CC_SHN_MASK      (0x3U << NVME_CC_SHN_SHIFT)
#define NVME_CC_IOSQES_SHIFT  16
#define NVME_CC_IOSQES_MASK   (0xFU << NVME_CC_IOSQES_SHIFT)
#define NVME_CC_IOCQES_SHIFT  20
#define NVME_CC_IOCQES_MASK   (0xFU << NVME_CC_IOCQES_SHIFT)

/* SHN Values */
#define NVME_SHN_NONE         0  /* No Shutdown */
#define NVME_SHN_NORMAL       1  /* Normal Shutdown */
#define NVME_SHN_ABRUPT       2  /* Abrupt Shutdown */

/* CSTS Register Bit Definitions */
#define NVME_CSTS_RDY_SHIFT   0
#define NVME_CSTS_RDY_MASK    (0x1U << NVME_CSTS_RDY_SHIFT)
#define NVME_CSTS_CFS_SHIFT   1
#define NVME_CSTS_CFS_MASK    (0x1U << NVME_CSTS_CFS_SHIFT)
#define NVME_CSTS_SHST_SHIFT  2
#define NVME_CSTS_SHST_MASK   (0x3U << NVME_CSTS_SHST_SHIFT)
#define NVME_CSTS_NSSRO_SHIFT 4
#define NVME_CSTS_NSSRO_MASK  (0x1U << NVME_CSTS_NSSRO_SHIFT)
#define NVME_CSTS_PP_SHIFT    5
#define NVME_CSTS_PP_MASK     (0x1U << NVME_CSTS_PP_SHIFT)
#define NVME_CSTS_ST_SHIFT    6
#define NVME_CSTS_ST_MASK     (0x1U << NVME_CSTS_ST_SHIFT)

/* SHST Values */
#define NVME_SHST_NONE        0  /* No Shutdown */
#define NVME_SHST_IN_PROGRESS 1  /* Shutdown Processing */
#define NVME_SHST_COMPLETE    2  /* Shutdown Complete */

/* AQA Register Bit Definitions */
#define NVME_AQA_ASQS_SHIFT   0
#define NVME_AQA_ASQS_MASK    (0xFFFU << NVME_AQA_ASQS_SHIFT)
#define NVME_AQA_ACQS_SHIFT   16
#define NVME_AQA_ACQS_MASK    (0xFFFU << NVME_AQA_ACQS_SHIFT)

/* NSSR Register Magic Value */
#define NVME_NSSR_MAGIC       0x4E564D45  /* "NVMe" */

/* Doorbell Stride (in bytes) */
#define NVME_DB_STRIDE        8  /* 4 bytes for SQ Tail + 4 bytes for CQ Head */

/* Admin Command Opcodes */
#define NVME_ADMIN_DELETE_IO_SQ    0x00
#define NVME_ADMIN_CREATE_IO_SQ    0x01
#define NVME_ADMIN_GET_LOG_PAGE    0x02
#define NVME_ADMIN_DELETE_IO_CQ    0x04
#define NVME_ADMIN_CREATE_IO_CQ    0x05
#define NVME_ADMIN_IDENTIFY        0x06
#define NVME_ADMIN_ABORT           0x08
#define NVME_ADMIN_SET_FEATURES    0x09
#define NVME_ADMIN_GET_FEATURES    0x0A
#define NVME_ADMIN_ASYNC_EVENT     0x0C
#define NVME_ADMIN_NS_MANAGEMENT   0x0D
#define NVME_ADMIN_FW_ACTIVATE     0x10
#define NVME_ADMIN_FW_DOWNLOAD     0x11
#define NVME_ADMIN_DEV_SELF_TEST   0x14
#define NVME_ADMIN_NS_ATTACHMENT   0x15
#define NVME_ADMIN_KEEP_ALIVE      0x18
#define NVME_ADMIN_DIRECTIVE_SEND  0x19
#define NVME_ADMIN_DIRECTIVE_RECV  0x1A
#define NVME_ADMIN_VIRTUAL_MGMT    0x1C
#define NVME_ADMIN_NVME_MI_SEND    0x1D
#define NVME_ADMIN_NVME_MI_RECV    0x1E
#define NVME_ADMIN_CAPACITY_MGMT   0x20
#define NVME_ADMIN_LOCKDOWN        0x24
#define NVME_ADMIN_DOORBELL_BUFFER 0x7C
#define NVME_ADMIN_FORMAT_NVM      0x80
#define NVME_ADMIN_SECURITY_SEND   0x81
#define NVME_ADMIN_SECURITY_RECV   0x82
#define NVME_ADMIN_SANITIZE        0x84
#define NVME_ADMIN_GET_LBA_STATUS  0x86

/* Sanitize Action (CDW10 bits [2:0], NVMe spec §5.22) */
#define NVME_SANACT_EXIT_FAILURE   0x01
#define NVME_SANACT_BLOCK_ERASE    0x02
#define NVME_SANACT_OVERWRITE      0x03
#define NVME_SANACT_CRYPTO_ERASE   0x04

/* Get Log Page — Log Page Identifiers */
#define NVME_LID_ERROR_INFO        0x01
#define NVME_LID_SMART             0x02
#define NVME_LID_FW_SLOT           0x03
#define NVME_LID_TELEMETRY_HOST    0x07  /* host-initiated (REQ-174) */
#define NVME_LID_TELEMETRY_CTRL    0x08  /* controller-initiated (REQ-175) */
#define NVME_LID_VENDOR_COUNTERS   0xC0  /* vendor-specific counters (REQ-176) */

/* NVMe Telemetry Log Page header — 512 bytes. Maps the fields this
 * simulator populates out of the spec §5.14.1.7 / §5.14.1.8 layout.
 * Data Area 1 immediately follows and carries serialized tel_event
 * records (most recent first). */
struct nvme_telemetry_log_header {
    uint8_t  log_identifier;                 /* byte 0: 0x07 or 0x08 */
    uint8_t  reserved0[4];                   /* bytes 1-4 */
    uint8_t  ieee_oui[3];                    /* bytes 5-7 */
    uint16_t data_area_1_last_block;         /* bytes 8-9 (512-byte units) */
    uint16_t data_area_2_last_block;         /* bytes 10-11 */
    uint16_t data_area_3_last_block;         /* bytes 12-13 */
    uint8_t  reserved1[368];                 /* bytes 14-381 */
    uint8_t  host_gen_number;                /* byte 382: host-initiated gen */
    uint8_t  ctrl_data_available;            /* byte 383: 1=data present */
    uint8_t  ctrl_gen_number;                /* byte 384: ctrl-initiated gen */
    uint8_t  reserved2[127];                 /* bytes 385-511 */
} __attribute__((packed));

/* NVMe Vendor-Specific Log Page: internal counters snapshot (LID 0xC0).
 * Gives hfsss-ctrl and test harnesses a stable view of telemetry ring
 * state without having to parse the per-event Data Area. */
#define NVME_VENDOR_LOG_MAGIC  0x484C4754U  /* "HLGT" */
#define NVME_VENDOR_LOG_EVENT_TYPES 8       /* covers tel_event_type enum */

struct nvme_vendor_log_counters {
    uint32_t magic;                                   /* bytes 0-3 */
    uint32_t version;                                 /* bytes 4-7 */
    uint64_t total_events;                            /* bytes 8-15 */
    uint32_t events_in_ring;                          /* bytes 16-19 */
    uint32_t ring_head;                               /* bytes 20-23 */
    uint64_t events_by_type[NVME_VENDOR_LOG_EVENT_TYPES]; /* bytes 24-87 */
    uint8_t  reserved[40];                            /* bytes 88-127 */
} __attribute__((packed));

/* Error Information Log Page entry — 64 bytes per NVMe spec §5.14.1.1 */
#define NVME_ERROR_LOG_ENTRIES     64

struct nvme_error_log_entry {
    uint64_t error_count;
    uint16_t sq_id;
    uint16_t cmd_id;
    uint16_t status_field;      /* DNR:1 | SCT:3 | SC:8 (+ reserved) */
    uint16_t parm_err_loc;
    uint64_t lba;
    uint32_t nsid;
    uint8_t  vs;
    uint8_t  trtype;
    uint8_t  reserved[2];
    uint64_t cmd_specific;
    uint16_t trtype_specific;
    uint8_t  reserved2[22];
} __attribute__((packed));

/* NVM I/O Command Opcodes */
#define NVME_NVM_FLUSH             0x00
#define NVME_NVM_WRITE             0x01
#define NVME_NVM_READ              0x02
#define NVME_NVM_WRITE_UNCORRECTABLE 0x04
#define NVME_NVM_COMPARE           0x05
#define NVME_NVM_WRITE_ZEROES      0x08
#define NVME_NVM_DATASET_MANAGEMENT 0x09
#define NVME_CMD_DSM                0x09  /* Alias for Dataset Management */

/* Dataset Management / Deallocate (Trim) */
#define NVME_DSM_ATTR_DEALLOCATE    (1u << 2)

struct nvme_dsm_range {
    uint32_t attributes;   /* context attributes (ignored in simulation) */
    uint32_t nlb;          /* number of logical blocks */
    uint64_t slba;         /* starting LBA */
} __attribute__((packed));
#define NVME_NVM_VERIFY            0x0C
#define NVME_NVM_COPY              0x19
#define NVME_NVM_IO_SEND           0x79
#define NVME_NVM_IO_RECV           0x7D

/* Status Code Types */
#define NVME_STATUS_TYPE_GENERIC  0x00
#define NVME_STATUS_TYPE_CMD_SPEC 0x01
#define NVME_STATUS_TYPE_MEDIA_ERR 0x02
#define NVME_STATUS_TYPE_PATH_ERR  0x03

/* Generic Status Codes */
#define NVME_SC_SUCCESS                0x00
#define NVME_SC_INVALID_OPCODE         0x01
#define NVME_SC_INVALID_FIELD          0x02
#define NVME_SC_CMD_ID_CONFLICT        0x03
#define NVME_SC_DATA_TRANSFER_ERROR    0x04
#define NVME_SC_ABORTED_POWER_LOSS     0x05
#define NVME_SC_INTERNAL_DEVICE_ERROR  0x06
#define NVME_SC_CMD_ABORT_REQUESTED    0x07
#define NVME_SC_CMD_ABORTED_SQ_DELETED 0x08
#define NVME_SC_CMD_ABORTED_FAILED_FUSE 0x09
#define NVME_SC_CMD_ABORTED_MISSING_FUSE 0x0A
#define NVME_SC_INVALID_NAMESPACE      0x0B
#define NVME_SC_CMD_SEQ_ERROR          0x0C
#define NVME_SC_INVALID_SGL_SEG_DESC   0x0D
#define NVME_SC_INVALID_SGL_COUNT      0x0E
#define NVME_SC_DATA_SGL_LEN_INVALID   0x0F
#define NVME_SC_METADATA_SGL_LEN_INVALID 0x10
#define NVME_SC_SGL_DESC_TYPE_INVALID  0x11
#define NVME_SC_INVALID_CMB_USE        0x12
#define NVME_SC_PRP_OFFSET_INVALID     0x13
#define NVME_SC_ATOMIC_WRITE_UNIT_EXCEEDED 0x14
#define NVME_SC_OP_DENIED              0x16
#define NVME_SC_INVALID_SGL_OFFSET     0x17
#define NVME_SC_CONFLICTING_ATTRS      0x18
#define NVME_SC_INSUFFICIENT_RESOURCES 0x19
#define NVME_SC_SKIPPED                 0x7F
#define NVME_SC_CAP_EXCEEDED            0x80
#define NVME_SC_NAMESPACE_NOT_READY     0x81

/* NVMe Submission Queue Entry */
struct nvme_sq_entry {
    /* Dword 0: Command DWORD 0 */
    u8  opcode;
    u8  flags;
    u16 command_id;

    /* Dword 1: Namespace Identifier */
    u32 nsid;

    /* Dword 2-3: Reserved */
    u32 cdw2;
    u32 cdw3;

    /* Dword 4-5: Metadata Pointer */
    u64 mptr;

    /* Dword 6-9: Data Pointer (PRP1/PRP2 or SGL) */
    union {
        struct {
            u64 prp1;
            u64 prp2;
        } prp;
        u8 sgl[16];
    } dp;

    /* Dword 10-15: Command Specific Dwords */
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} __attribute__((packed));

/* NVMe Completion Queue Entry */
struct nvme_cq_entry {
    /* Dword 0: Command Specific */
    u32 cdw0;

    /* Dword 1: Reserved */
    u32 rsvd1;

    /* Dword 2: SQ Head Pointer, SQ Identifier */
    u16 sq_head;
    u16 sq_id;

    /* Dword 3: Command Identifier, Status */
    u16 command_id;
    u16 status;
} __attribute__((packed));

/* Status Field Helpers */
#define NVME_STATUS_PHASE_BIT   0x0001
#define NVME_STATUS_SC_MASK     0x07FE
#define NVME_STATUS_SC_SHIFT    1
#define NVME_STATUS_SCT_MASK    0x0800
#define NVME_STATUS_SCT_SHIFT   11
#define NVME_STATUS_MORE        0x4000
#define NVME_STATUS_DNR         0x8000

#define NVME_BUILD_STATUS(sc, sct) \
    (((sc) << NVME_STATUS_SC_SHIFT) | \
     ((sct) << NVME_STATUS_SCT_SHIFT))

/* NVMe Controller Registers */
struct nvme_controller_regs {
    union {
        struct {
            u64 cap;      /* 0x00: CAP */
            u32 vs;       /* 0x08: VS */
            u32 intms;    /* 0x0C: INTMS */
            u32 intmc;    /* 0x10: INTMC */
            u32 cc;       /* 0x14: CC */
            u32 rsvd1;    /* 0x18: Reserved */
            u32 csts;     /* 0x1C: CSTS */
            u32 nssr;     /* 0x20: NSSR */
            u32 aqa;      /* 0x24: AQA */
            u64 asq;      /* 0x28: ASQ */
            u64 acq;      /* 0x30: ACQ */
            u32 cmbloc;   /* 0x38: CMBLOC */
            u32 cmbsz;    /* 0x3C: CMBSZ */
            u32 bpinfo;   /* 0x40: BPINFO */
            u32 bprsel;   /* 0x44: BPRSEL */
            u64 bpmbl;    /* 0x48: BPMBL */
            u8  rsvd2[0x1000 - 0x50];
        };
        u8 raw[0x1000];
    } regs;

    /* 0x1000+: Doorbell Registers */
    struct {
        u32 sq_tail;
        u32 cq_head;
    } doorbell[64];
} __attribute__((packed));

/* Identify Controller Data Structure (partially implemented) */
struct nvme_identify_ctrl {
    u16 vid;          /* 0x00: PCI Vendor ID */
    u16 ssvid;        /* 0x02: PCI Subsystem Vendor ID */
    char sn[20];       /* 0x04: Serial Number */
    char mn[40];       /* 0x18: Model Number */
    char fr[8];        /* 0x40: Firmware Revision */
    u8  rab;          /* 0x48: Recommended Arbitration Burst */
    u8  ieee[3];      /* 0x49: IEEE OUI */
    u8  cmic;         /* 0x4C: Controller Multi-Path I/O and Namespace Sharing Capabilities */
    u8  mdts;         /* 0x4D: Maximum Data Transfer Size */
    u16 cntlid;       /* 0x4E: Controller ID */
    u32 ver;          /* 0x50: Version */
    u32 rtd3r;        /* 0x54: RTD3 Resume Latency */
    u32 rtd3e;        /* 0x58: RTD3 Entry Latency */
    u32 oaes;         /* 0x5C: Optional Asynchronous Events Supported */
    u32 ctratt;       /* 0x60: Controller Attributes */
    u16 rrls;         /* 0x64: Read Recovery Levels Supported */
    u8  rsvd1[9];     /* 0x66: Reserved */
    u8  cntrltype;    /* 0x6F: Controller Type */
    u8  fguid[16];    /* 0x70: FRU GUID */
    u16 crdt1;        /* 0x80: Command Retry Delay Time 1 */
    u16 crdt2;        /* 0x82: Command Retry Delay Time 2 */
    u16 crdt3;        /* 0x84: Command Retry Delay Time 3 */
    u8  rsvd2[106];   /* 0x86: Reserved */
    u16 oacs;         /* 0xF0: Optional Admin Command Support */
    u8  acl;          /* 0xF2: Abort Command Limit */
    u8  aerl;         /* 0xF3: Asynchronous Event Request Limit */
    u8  frmw;         /* 0xF4: Firmware Updates */
    u8  lpa;          /* 0xF5: Log Page Attributes */
    u8  elpe;         /* 0xF6: Error Log Page Entries */
    u8  npss;         /* 0xF7: Number of Power States Support */
    u8  avscc;        /* 0xF8: Admin Vendor Specific Command Configuration */
    u8  apsta;        /* 0xF9: Autonomous Power State Transition Attributes */
    u16 wctemp;       /* 0xFA: Warning Composite Temperature Threshold */
    u16 cctemp;       /* 0xFC: Critical Composite Temperature Threshold */
    u16 mtfa;         /* 0xFE: Maximum Time for Firmware Activation */
    u32 hmpre;        /* 0x100: Host Memory Buffer Preferred Size */
    u32 hmmin;        /* 0x104: Host Memory Buffer Minimum Size */
    u8  tnvmcap[16];  /* 0x108: Total NVM Capacity */
    u8  unvmcap[16];  /* 0x118: Unallocated NVM Capacity */
    u32 rpmbs;        /* 0x128: Replay Protected Memory Block Support */
    u16 edstt;        /* 0x12C: Extended Device Self-Test Time */
    u8  dsto;         /* 0x12E: Device Self-Test Options */
    u8  fwug;         /* 0x12F: Firmware Update Granularity */
    u16 kas;          /* 0x130: Keep Alive Support */
    u16 hctma;        /* 0x132: Host Controlled Thermal Management Attributes */
    u16 mntmt;        /* 0x134: Minimum Thermal Management Temperature */
    u16 mxtmt;        /* 0x136: Maximum Thermal Management Temperature */
    u32 sanicap;      /* 0x138: Sanitize Capabilities */
    u32 hmminds;      /* 0x13C: Host Memory Buffer Minimum Descriptor Entry Size */
    u16 hmmaxd;       /* 0x140: Host Memory Maximum Descriptors Entries */
    u16 nsetidmax;    /* 0x142: NVM Set Identifier Maximum */
    u16 endgidmax;    /* 0x144: Endurance Group Identifier Maximum */
    u8  anatt;        /* 0x146: ANA Attributes */
    u8  anacap;       /* 0x147: ANA Capabilities */
    u32 anagrpmax;    /* 0x148: ANA Group Identifier Maximum */
    u32 nanagrpid;    /* 0x14C: Number of ANA Group Identifiers */
    u32 pels;         /* 0x150: Persistent Event Log Size */
    u8  rsvd3[156];   /* 0x154: Reserved */
    u8  sqes;         /* 0x1F0: Submission Queue Entry Size */
    u8  cqes;         /* 0x1F1: Completion Queue Entry Size */
    u16 maxcmd;       /* 0x1F2: Maximum Outstanding Commands */
    u32 nn;           /* 0x1F4: Number of Namespaces */
    u16 oncs;         /* 0x1F8: Optional NVM Command Support */
    u16 fuses;        /* 0x1FA: Fused Operation Support */
    u8  fna;          /* 0x1FC: Format NVM Attributes */
    u8  vwc;          /* 0x1FD: Volatile Write Cache */
    u16 awun;         /* 0x1FE: Atomic Write Unit Normal */
    u16 awupf;        /* 0x200: Atomic Write Unit Power Fail */
    u8  nvscc;        /* 0x202: NVM Vendor Specific Command Configuration */
    u8  rsvd4;        /* 0x203: Reserved */
    u16 acwu;         /* 0x204: Atomic Compare & Write Unit */
    u8  rsvd5[2];     /* 0x206: Reserved */
    u32 sgls;         /* 0x208: SGL Support */
    u8  rsvd6[228];   /* 0x20C: Reserved */
    u8  subnqn[256];  /* 0x2F8: NVM Subsystem NVMe Qualified Name */
    u8  rsvd7[768];   /* 0x3F8: Reserved */
    u8  psd[32][32];  /* 0x700: Power State Descriptors */
    u8  vs[1024];     /* 0x1000: Vendor Specific */
} __attribute__((packed));

/* Identify Namespace Data Structure (partially implemented) */
struct nvme_identify_ns {
    u64 nsze;         /* 0x00: Namespace Size */
    u64 ncap;         /* 0x08: Namespace Capacity */
    u64 nuse;         /* 0x10: Namespace Utilization */
    u8  nsfeat;       /* 0x18: Namespace Features */
    u8  nlbaf;        /* 0x19: Number of LBA Formats */
    u8  flbas;        /* 0x1A: Formatted LBA Size */
    u8  mc;           /* 0x1B: Metadata Capabilities */
    u8  dpc;          /* 0x1C: End-to-end Data Protection Capabilities */
    u8  dps;          /* 0x1D: End-to-end Data Protection Type Settings */
    u8  nmic;         /* 0x1E: Namespace Multi-path I/O and Namespace Sharing Capabilities */
    u8  rescap;       /* 0x1F: Reservation Capabilities */
    u8  fpi;          /* 0x20: Format Progress Indicator */
    u8  dlfeat;       /* 0x21: Deallocate Logical Block Features */
    u16 nawun;        /* 0x22: Namespace Atomic Write Unit Normal */
    u16 nawupf;       /* 0x24: Namespace Atomic Write Unit Power Fail */
    u16 nacwu;        /* 0x26: Namespace Atomic Compare & Write Unit */
    u16 nabsn;        /* 0x28: Namespace Atomic Boundary Size Normal */
    u16 nabo;         /* 0x2A: Namespace Atomic Boundary Offset */
    u16 nabspf;       /* 0x2C: Namespace Atomic Boundary Size Power Fail */
    u16 noiob;        /* 0x2E: Namespace Optimal I/O Boundary */
    u64 nvmcap[2];    /* 0x30: NVM Capacity */
    u16 npwg;         /* 0x40: Namespace Preferred Write Granularity */
    u16 npwa;         /* 0x42: Namespace Preferred Write Alignment */
    u16 npdg;         /* 0x44: Namespace Preferred Deallocate Granularity */
    u16 npda;         /* 0x46: Namespace Preferred Deallocate Alignment */
    u16 nows;         /* 0x48: Namespace Optimal Write Size */
    u8  rsvd1[18];    /* 0x4A: Reserved */
    u8  anagrpid[4];  /* 0x5C: ANA Group Identifier */
    u8  rsvd2[3];     /* 0x60: Reserved */
    u8  attr;         /* 0x63: Namespace Attributes */
    u16 nvmsetid;     /* 0x64: NVM Set Identifier */
    u16 endgid;       /* 0x66: Endurance Group Identifier */
    u8  nguid[16];    /* 0x68: Namespace Globally Unique Identifier */
    u64 eui64;        /* 0x78: IEEE Extended Unique Identifier */
    struct {
        u32 ms : 16;
        u32 lbads : 8;
        u32 rp : 2;
        u32 rsvd : 6;
    } lbaf[16];        /* 0x80: LBA Format Support */
    u8  rsvd3[192];   /* 0xC0: Reserved */
    u8  vs[3712];     /* 0x180: Vendor Specific */
} __attribute__((packed));

_Static_assert(sizeof(struct nvme_identify_ns) == 4096,
               "NVMe Identify Namespace data structure must be 4096 bytes per spec");

/* NVMe Controller Context */
struct nvme_ctrl_ctx {
    /* PCI Device Context */
    struct pci_dev_ctx pci_dev;

    /* Controller Registers */
    struct nvme_controller_regs regs;

    /* Configuration */
    bool enabled;
    u32 page_size;
    u32 sq_entry_size;
    u32 cq_entry_size;

    /* Doorbell values */
    u32 sq_tail[64];
    u32 cq_head[64];

    /* Lock for register access */
    struct mutex lock;

    /* Backend callback to user-space daemon */
    void *backend_data;
    int (*backend_cmd_handler)(void *data, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl);
};

/* Function Prototypes */
int nvme_ctrl_init(struct nvme_ctrl_ctx *ctrl);
void nvme_ctrl_cleanup(struct nvme_ctrl_ctx *ctrl);
int nvme_ctrl_reg_read(struct nvme_ctrl_ctx *ctrl, u64 offset, u64 *value, u32 size);
int nvme_ctrl_reg_write(struct nvme_ctrl_ctx *ctrl, u64 offset, u64 value, u32 size);
int nvme_ctrl_process_admin_cmd(struct nvme_ctrl_ctx *ctrl, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl);
int nvme_ctrl_process_io_cmd(struct nvme_ctrl_ctx *ctrl, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl);
int nvme_ctrl_process_identify(struct nvme_sq_entry *cmd, void *data, size_t data_len);
void nvme_build_identify_ctrl(struct nvme_identify_ctrl *id);
void nvme_build_identify_ns(struct nvme_identify_ns *id, u32 nsid);

#endif /* __HFSSS_NVME_H */
