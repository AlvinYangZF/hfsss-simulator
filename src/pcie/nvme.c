#include "pcie/nvme.h"
#include <stdlib.h>
#include <string.h>

static void nvme_regs_init_default(struct nvme_controller_regs *regs)
{
    /* CAP: Controller Capabilities */
    regs->regs.cap = 0;
    regs->regs.cap |= (65535ULL << NVME_CAP_MQES_SHIFT);    /* MQES: 65535 */
    regs->regs.cap |= (0ULL << NVME_CAP_CQR_SHIFT);       /* CQR: 0 */
    regs->regs.cap |= (3ULL << NVME_CAP_AMS_SHIFT);       /* AMS: Round Robin + WRR */
    regs->regs.cap |= (20ULL << NVME_CAP_TO_SHIFT);       /* TO: 20 (10 seconds) */
    regs->regs.cap |= (0ULL << NVME_CAP_DSTRD_SHIFT);     /* DSTRD: 0 */
    regs->regs.cap |= (1ULL << NVME_CAP_NSSRS_SHIFT);     /* NSSRS: 1 */
    regs->regs.cap |= (1ULL << NVME_CAP_CSS_SHIFT);       /* CSS: NVM Command Set */
    regs->regs.cap |= (0ULL << NVME_CAP_MPSMIN_SHIFT);    /* MPSMIN: 0 (4KB) */
    regs->regs.cap |= (4ULL << NVME_CAP_MPSMAX_SHIFT);    /* MPSMAX: 4 (64KB) */

    /* VS: Version */
    regs->regs.vs = NVME_VERSION_2_0;

    /* INTMS/INTMC: Interrupt Mask */
    regs->regs.intms = 0;
    regs->regs.intmc = 0;

    /* CC: Controller Configuration */
    regs->regs.cc = 0;

    /* CSTS: Controller Status */
    regs->regs.csts = 0;

    /* NSSR: NVM Subsystem Reset */
    regs->regs.nssr = 0;

    /* AQA: Admin Queue Attributes */
    regs->regs.aqa = 0;

    /* ASQ/ACQ: Admin Queue Base Addresses */
    regs->regs.asq = 0;
    regs->regs.acq = 0;

    /* CMBLOC/CMBSZ: Controller Memory Buffer */
    regs->regs.cmbloc = 0;
    regs->regs.cmbsz = 0;

    /* BPINFO/BPRSEL/BPMBL: Boot Partition */
    regs->regs.bpinfo = 0;
    regs->regs.bprsel = 0;
    regs->regs.bpmbl = 0;

    /* Doorbells */
    memset(regs->doorbell, 0, sizeof(regs->doorbell));
}

int nvme_ctrl_init(struct nvme_ctrl_ctx *ctrl)
{
    int ret;

    if (!ctrl) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->enabled = false;
    ctrl->page_size = 4096;
    ctrl->sq_entry_size = 64;
    ctrl->cq_entry_size = 16;

    memset(ctrl->sq_tail, 0, sizeof(ctrl->sq_tail));
    memset(ctrl->cq_head, 0, sizeof(ctrl->cq_head));

    ret = mutex_init(&ctrl->lock);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize PCI device */
    ret = pci_dev_init(&ctrl->pci_dev);
    if (ret != HFSSS_OK) {
        mutex_cleanup(&ctrl->lock);
        return ret;
    }

    /* Initialize registers */
    nvme_regs_init_default(&ctrl->regs);

    return HFSSS_OK;
}

void nvme_ctrl_cleanup(struct nvme_ctrl_ctx *ctrl)
{
    if (!ctrl) {
        return;
    }

    pci_dev_cleanup(&ctrl->pci_dev);
    mutex_cleanup(&ctrl->lock);
    memset(ctrl, 0, sizeof(*ctrl));
}

int nvme_ctrl_reg_read(struct nvme_ctrl_ctx *ctrl, u64 offset, u64 *value, u32 size)
{
    u64 val = 0;

    if (!ctrl || !value) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctrl->lock, 0);

    if (offset < NVME_REG_DBS) {
        /* Controller registers */
        switch (offset) {
        case NVME_REG_CAP:
            val = ctrl->regs.regs.cap;
            break;
        case NVME_REG_VS:
            val = ctrl->regs.regs.vs;
            break;
        case NVME_REG_INTMS:
            val = ctrl->regs.regs.intms;
            break;
        case NVME_REG_INTMC:
            val = ctrl->regs.regs.intmc;
            break;
        case NVME_REG_CC:
            val = ctrl->regs.regs.cc;
            break;
        case NVME_REG_CSTS:
            val = ctrl->regs.regs.csts;
            break;
        case NVME_REG_NSSR:
            val = ctrl->regs.regs.nssr;
            break;
        case NVME_REG_AQA:
            val = ctrl->regs.regs.aqa;
            break;
        case NVME_REG_ASQ:
            val = ctrl->regs.regs.asq;
            break;
        case NVME_REG_ACQ:
            val = ctrl->regs.regs.acq;
            break;
        case NVME_REG_CMBLOC:
            val = ctrl->regs.regs.cmbloc;
            break;
        case NVME_REG_CMBSZ:
            val = ctrl->regs.regs.cmbsz;
            break;
        case NVME_REG_BPINFO:
            val = ctrl->regs.regs.bpinfo;
            break;
        case NVME_REG_BPRSEL:
            val = ctrl->regs.regs.bprsel;
            break;
        case NVME_REG_BPMBL:
            val = ctrl->regs.regs.bpmbl;
            break;
        default:
            val = 0;
            break;
        }

        /* Truncate based on size */
        if (size == 1) val &= 0xFF;
        else if (size == 2) val &= 0xFFFF;
        else if (size == 4) val &= 0xFFFFFFFF;
    } else if (offset < NVME_REG_DBS + 64 * 8) {
        /* Doorbell registers - return 0 on reads */
        val = 0;
    }

    *value = val;

    mutex_unlock(&ctrl->lock);

    return HFSSS_OK;
}

static void nvme_handle_cc_write(struct nvme_ctrl_ctx *ctrl, u32 new_cc)
{
    u32 old_cc = ctrl->regs.regs.cc;
    bool old_en = (old_cc & NVME_CC_EN_MASK) != 0;
    bool new_en = (new_cc & NVME_CC_EN_MASK) != 0;

    if (!old_en && new_en) {
        /* Enable controller */
        u32 mps = (new_cc & NVME_CC_MPS_MASK) >> NVME_CC_MPS_SHIFT;
        u32 iosqes = (new_cc & NVME_CC_IOSQES_MASK) >> NVME_CC_IOSQES_SHIFT;
        u32 iocqes = (new_cc & NVME_CC_IOCQES_MASK) >> NVME_CC_IOCQES_SHIFT;

        ctrl->page_size = 4096 << mps;
        ctrl->sq_entry_size = 1 << iosqes;
        ctrl->cq_entry_size = 1 << iocqes;
        ctrl->enabled = true;

        /* Set RDY bit */
        ctrl->regs.regs.csts |= NVME_CSTS_RDY_MASK;
    } else if (old_en && !new_en) {
        /* Disable controller */
        ctrl->enabled = false;
        ctrl->regs.regs.csts &= ~NVME_CSTS_RDY_MASK;
    }

    ctrl->regs.regs.cc = new_cc;
}

int nvme_ctrl_reg_write(struct nvme_ctrl_ctx *ctrl, u64 offset, u64 value, u32 size)
{
    if (!ctrl) {
        return HFSSS_ERR_INVAL;
    }

    mutex_lock(&ctrl->lock, 0);

    if (offset < NVME_REG_DBS) {
        /* Controller registers */
        switch (offset) {
        case NVME_REG_INTMS:
            ctrl->regs.regs.intms |= (u32)value;
            break;
        case NVME_REG_INTMC:
            ctrl->regs.regs.intms &= ~(u32)value;
            break;
        case NVME_REG_CC:
            nvme_handle_cc_write(ctrl, (u32)value);
            break;
        case NVME_REG_NSSR:
            if ((u32)value == NVME_NSSR_MAGIC) {
            /* NVM Subsystem Reset */
            nvme_regs_init_default(&ctrl->regs);
            ctrl->enabled = false;
            memset(ctrl->sq_tail, 0, sizeof(ctrl->sq_tail));
            memset(ctrl->cq_head, 0, sizeof(ctrl->cq_head));
        }
            break;
        case NVME_REG_AQA:
            ctrl->regs.regs.aqa = (u32)value;
            break;
        case NVME_REG_ASQ:
            if (size == 4) {
                ctrl->regs.regs.asq = (ctrl->regs.regs.asq & 0xFFFFFFFF00000000ULL) | (u32)value;
            } else {
                ctrl->regs.regs.asq = value;
            }
            break;
        case NVME_REG_ACQ:
            if (size == 4) {
                ctrl->regs.regs.acq = (ctrl->regs.regs.acq & 0xFFFFFFFF00000000ULL) | (u32)value;
            } else {
                ctrl->regs.regs.acq = value;
            }
            break;
        case NVME_REG_BPRSEL:
            ctrl->regs.regs.bprsel = (u32)value;
            break;
        case NVME_REG_BPMBL:
            if (size == 4) {
                ctrl->regs.regs.bpmbl = (ctrl->regs.regs.bpmbl & 0xFFFFFFFF00000000ULL) | (u32)value;
            } else {
                ctrl->regs.regs.bpmbl = value;
            }
            break;
        default:
            break;
        }
    } else if (offset < NVME_REG_DBS + 64 * 8) {
        /* Doorbell registers */
        u32 db_idx = (offset - NVME_REG_DBS) / 8;
        u32 db_offset = (offset - NVME_REG_DBS) % 8;

        if (db_idx < 64) {
            if (db_offset == 0) {
                /* SQ Tail Doorbell */
                ctrl->sq_tail[db_idx] = (u32)value;
            } else if (db_offset == 4) {
                /* CQ Head Doorbell */
                ctrl->cq_head[db_idx] = (u32)value;
            }
        }
    }

    mutex_unlock(&ctrl->lock);

    return HFSSS_OK;
}

void nvme_build_identify_ctrl(struct nvme_identify_ctrl *id)
{
    memset(id, 0, sizeof(*id));

    /* PCI Vendor/Subsystem IDs */
    id->vid = HFSSS_VENDOR_ID;
    id->ssvid = HFSSS_SUBSYSTEM_VENDOR_ID;

    /* Serial/Model/Firmware */
    strncpy(id->sn, "HFSSS0001", sizeof(id->sn));
    strncpy(id->mn, "HFSSS NVMe SSD", sizeof(id->mn));
    strncpy(id->fr, "1.0", sizeof(id->fr));

    /* IEEE OUI (example) */
    id->ieee[0] = 0x00;
    id->ieee[1] = 0x1A;
    id->ieee[2] = 0x2B;

    /* Version */
    id->ver = NVME_VERSION_2_0;

    /* Controller Type */
    id->cntrltype = 0x01;  /* NVMe Controller */

    /* Optional Admin Command Support */
    id->oacs = 0x0003;  /* Firmware Activate/Download, Format NVM */

    /* Abort Command Limit */
    id->acl = 3;

    /* Asynchronous Event Request Limit */
    id->aerl = 7;

    /* Firmware Updates */
    id->frmw = 0x03;  /* 2 slots, no activation without reset */

    /* Log Page Attributes */
    id->lpa = 0x03;  /* SMART/Health log, Error log */

    /* Error Log Page Entries */
    id->elpe = 63;

    /* Number of Power States Support */
    id->npss = 1;

    /* SQ/CQ Entry Sizes */
    id->sqes = 0x64;  /* 64 bytes SQ, 16 bytes CQ */
    id->cqes = 0x44;

    /* Maximum Outstanding Commands */
    id->maxcmd = 1024;

    /* Number of Namespaces */
    id->nn = 1;

    /* Optional NVM Command Support */
    id->oncs = 0x001E;  /* Write Uncorrectable, Compare, Write Zeroes, Dataset Mgmt */

    /* Volatile Write Cache */
    id->vwc = 0x01;

    /* Atomic Write Unit Normal */
    id->awun = 0x0000;

    /* NVM Subsystem NVMe Qualified Name */
    strncpy((char *)id->subnqn, "nqn.2026-03.com.hfsss:nvme-ssd", sizeof(id->subnqn));
}

void nvme_build_identify_ns(struct nvme_identify_ns *id, u32 nsid)
{
    memset(id, 0, sizeof(*id));

    if (nsid == 1 || nsid == 0) {
        /* Namespace 1 */
        /* 1GB namespace (4KB pages) */
        id->nsze = (1ULL << 28);  /* 268,435,456 LBAs */
        id->ncap = (1ULL << 28);
        id->nuse = 0;

        /* Namespace Features */
        id->nsfeat = 0x01;  /* Thin provisioning */

        /* Number of LBA Formats */
        id->nlbaf = 1;

        /* Formatted LBA Size */
        id->flbas = 0x00;  /* LBA format 0 */

        /* LBA Format 0: 4KB, no metadata */
        id->lbaf[0].ms = 0;
        id->lbaf[0].lbads = 12;  /* 2^12 = 4096 bytes */
        id->lbaf[0].rp = 0;
    }
}

int nvme_ctrl_process_admin_cmd(struct nvme_ctrl_ctx *ctrl, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl)
{
    u16 status = 0;

    memset(cpl, 0, sizeof(*cpl));

    cpl->command_id = cmd->command_id;
    cpl->sq_id = 0;

    switch (cmd->opcode) {
    case NVME_ADMIN_IDENTIFY:
        /* Identify command */
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_SET_FEATURES:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_GET_FEATURES:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_CREATE_IO_SQ:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_CREATE_IO_CQ:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_DELETE_IO_SQ:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_DELETE_IO_CQ:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_GET_LOG_PAGE:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_ASYNC_EVENT:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_KEEP_ALIVE:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_ADMIN_FORMAT_NVM:
        status = NVME_SC_SUCCESS;
        break;

    default:
        /* Invalid Opcode */
        status = NVME_BUILD_STATUS(NVME_SC_INVALID_OPCODE, NVME_STATUS_TYPE_GENERIC);
        break;
    }

    cpl->status = status;

    return HFSSS_OK;
}

int nvme_ctrl_process_io_cmd(struct nvme_ctrl_ctx *ctrl, struct nvme_sq_entry *cmd, struct nvme_cq_entry *cpl)
{
    u16 status = 0;

    memset(cpl, 0, sizeof(*cpl));

    cpl->command_id = cmd->command_id;
    cpl->sq_id = cmd->flags & 0xFFFF;

    switch (cmd->opcode) {
    case NVME_NVM_FLUSH:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_NVM_WRITE:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_NVM_READ:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_NVM_WRITE_ZEROES:
        status = NVME_SC_SUCCESS;
        break;

    case NVME_NVM_DATASET_MANAGEMENT:
        status = NVME_SC_SUCCESS;
        break;

    default:
        status = NVME_BUILD_STATUS(NVME_SC_INVALID_OPCODE, NVME_STATUS_TYPE_GENERIC);
        break;
    }

    cpl->status = status;

    return HFSSS_OK;
}
