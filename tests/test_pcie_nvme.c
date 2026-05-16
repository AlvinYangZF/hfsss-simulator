#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pcie/pcie_nvme.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        tests_run++;                                                                                                   \
        if (cond) {                                                                                                    \
            printf("  [PASS] %s\n", msg);                                                                              \
            tests_passed++;                                                                                            \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            tests_failed++;                                                                                            \
        }                                                                                                              \
    } while (0)

static void print_separator(void)
{
    printf("========================================\n");
}

/* PCI Device Tests */
static int test_pci_dev(void)
{
    printf("\n=== PCI Device Tests ===\n");

    struct pci_dev_ctx dev;
    u32 val;
    int ret;

    ret = pci_dev_init(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "pci_dev_init should succeed");

    /* Check Vendor/Device ID */
    ret = pci_dev_cfg_read(&dev, 0x00, &val, 2);
    TEST_ASSERT(ret == HFSSS_OK, "pci_dev_cfg_read should succeed");
    TEST_ASSERT(val == HFSSS_VENDOR_ID, "Vendor ID should be correct");

    ret = pci_dev_cfg_read(&dev, 0x02, &val, 2);
    TEST_ASSERT(val == HFSSS_DEVICE_ID, "Device ID should be correct");

    /* Check Class Code */
    ret = pci_dev_cfg_read(&dev, 0x08, &val, 1);
    TEST_ASSERT(val == HFSSS_REVISION_ID, "Revision ID should be correct");

    ret = pci_dev_cfg_read(&dev, 0x0A, &val, 1);
    TEST_ASSERT(val == PCI_CLASS_SUBCLASS_NVME, "Subclass should be NVMe");

    ret = pci_dev_cfg_read(&dev, 0x0B, &val, 1);
    TEST_ASSERT(val == PCI_CLASS_CODE_STORAGE, "Class should be Storage");

    /* Check Capabilities Pointer */
    ret = pci_dev_cfg_read(&dev, 0x34, &val, 1);
    TEST_ASSERT(val == 0x40, "Capabilities pointer should be at 0x40");

    /* Test Write to Command Register */
    ret = pci_dev_cfg_write(&dev, 0x04, PCI_CMD_MEM_EN | PCI_CMD_BUS_MASTER, 2);
    TEST_ASSERT(ret == HFSSS_OK, "pci_dev_cfg_write should succeed");

    pci_dev_cleanup(&dev);
    TEST_ASSERT(true, "pci_dev_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(pci_dev_init(NULL) == HFSSS_ERR_INVAL, "pci_dev_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* NVMe Controller Tests */
static int test_nvme_ctrl(void)
{
    printf("\n=== NVMe Controller Tests ===\n");

    struct nvme_ctrl_ctx ctrl;
    u64 val64;
    int ret;

    ret = nvme_ctrl_init(&ctrl);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_init should succeed");

    /* Read CAP register */
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_CAP, &val64, 8);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_reg_read CAP should succeed");
    TEST_ASSERT((val64 & NVME_CAP_MQES_MASK) != 0, "CAP.MQES should be non-zero");

    /* Read VS register */
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_VS, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_reg_read VS should succeed");
    TEST_ASSERT((u32)val64 == NVME_VERSION_2_0, "VS should be 2.0");

    /* Write and read AQA register */
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_AQA, 0x00FF00FF, 4);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_ctrl_reg_write AQA should succeed");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_AQA, &val64, 4);
    TEST_ASSERT((u32)val64 == 0x00FF00FF, "AQA should match");

    TEST_ASSERT(nvme_ctrl_reg_read(NULL, NVME_REG_CAP, &val64, 8) == HFSSS_ERR_INVAL,
                "nvme_ctrl_reg_read NULL ctrl rejected");
    TEST_ASSERT(nvme_ctrl_reg_read(&ctrl, NVME_REG_CAP, NULL, 8) == HFSSS_ERR_INVAL,
                "nvme_ctrl_reg_read NULL value rejected");
    TEST_ASSERT(nvme_ctrl_reg_write(NULL, NVME_REG_CC, 0, 4) == HFSSS_ERR_INVAL,
                "nvme_ctrl_reg_write NULL ctrl rejected");

    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_INTMS, 0x3, 4);
    TEST_ASSERT(ret == HFSSS_OK, "INTMS write succeeds");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_INTMS, &val64, 4);
    TEST_ASSERT((u32)val64 == 0x3, "INTMS bits set");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_INTMC, 0x1, 4);
    TEST_ASSERT(ret == HFSSS_OK, "INTMC write succeeds");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_INTMS, &val64, 4);
    TEST_ASSERT((u32)val64 == 0x2, "INTMC clears selected INTMS bit");

    u32 cc_enable = NVME_CC_EN_MASK |
                    (0u << NVME_CC_MPS_SHIFT) |
                    (6u << NVME_CC_IOSQES_SHIFT) |
                    (4u << NVME_CC_IOCQES_SHIFT);
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_CC, cc_enable, 4);
    TEST_ASSERT(ret == HFSSS_OK && ctrl.enabled == true,
                "CC enable transitions controller to enabled");
    TEST_ASSERT(ctrl.page_size == 4096 && ctrl.sq_entry_size == 64 &&
                ctrl.cq_entry_size == 16,
                "CC enable derives page and queue entry sizes");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_CSTS, &val64, 4);
    TEST_ASSERT((val64 & NVME_CSTS_RDY_MASK) != 0, "CSTS.RDY set after enable");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_CC, 0, 4);
    TEST_ASSERT(ret == HFSSS_OK && ctrl.enabled == false,
                "CC disable transitions controller to disabled");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_CSTS, &val64, 4);
    TEST_ASSERT((val64 & NVME_CSTS_RDY_MASK) == 0, "CSTS.RDY clear after disable");

    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_CC, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0, "CC readback after disable");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_NSSR, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0, "NSSR default readback");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_CMBLOC, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0, "CMBLOC default readback");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_CMBSZ, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0, "CMBSZ default readback");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_BPINFO, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0, "BPINFO default readback");

    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_ASQ, 0x11223344, 4);
    TEST_ASSERT(ret == HFSSS_OK, "ASQ low dword write succeeds");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_ASQ, 0x5566778899AABBCCULL, 8);
    TEST_ASSERT(ret == HFSSS_OK, "ASQ qword write succeeds");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_ASQ, &val64, 8);
    TEST_ASSERT(val64 == 0x5566778899AABBCCULL, "ASQ qword readback matches");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_ACQ, 0xAABBCCDD, 4);
    TEST_ASSERT(ret == HFSSS_OK, "ACQ low dword write succeeds");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_ACQ, 0x0102030405060708ULL, 8);
    TEST_ASSERT(ret == HFSSS_OK, "ACQ qword write succeeds");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_ACQ, &val64, 8);
    TEST_ASSERT(val64 == 0x0102030405060708ULL, "ACQ qword readback matches");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_BPRSEL, 0x12345678, 4);
    TEST_ASSERT(ret == HFSSS_OK, "BPRSEL write succeeds");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_BPRSEL, &val64, 4);
    TEST_ASSERT((u32)val64 == 0x12345678, "BPRSEL readback matches");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_BPMBL, 0xCAFEBABE, 4);
    TEST_ASSERT(ret == HFSSS_OK, "BPMBL low dword write succeeds");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_BPMBL, 0x1111222233334444ULL, 8);
    TEST_ASSERT(ret == HFSSS_OK, "BPMBL qword write succeeds");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_BPMBL, &val64, 8);
    TEST_ASSERT(val64 == 0x1111222233334444ULL, "BPMBL qword readback matches");

    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_DBS + 8, 7, 4);
    TEST_ASSERT(ret == HFSSS_OK && ctrl.sq_tail[1] == 7,
                "SQ tail doorbell write captured");
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_DBS + 8 + 4, 3, 4);
    TEST_ASSERT(ret == HFSSS_OK && ctrl.cq_head[1] == 3,
                "CQ head doorbell write captured");
    ret = nvme_ctrl_reg_read(&ctrl, NVME_REG_DBS + 8, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && val64 == 0,
                "doorbell reads return zero");
    ret = nvme_ctrl_reg_read(&ctrl, 0x58, &val64, 1);
    TEST_ASSERT(ret == HFSSS_OK && val64 == 0,
                "unknown controller register reads as zero with byte mask");

    ctrl.sq_tail[1] = 9;
    ctrl.cq_head[1] = 9;
    ret = nvme_ctrl_reg_write(&ctrl, NVME_REG_NSSR, NVME_NSSR_MAGIC, 4);
    TEST_ASSERT(ret == HFSSS_OK && ctrl.enabled == false &&
                ctrl.sq_tail[1] == 0 && ctrl.cq_head[1] == 0,
                "NSSR magic resets controller and doorbells");

    /* Test Identify data */
    struct nvme_identify_ctrl id_ctrl;
    nvme_build_identify_ctrl(&id_ctrl);
    TEST_ASSERT(id_ctrl.vid == HFSSS_VENDOR_ID, "Identify VID should be correct");
    TEST_ASSERT(strncmp((char *)id_ctrl.mn, "HFSSS NVMe SSD", sizeof(id_ctrl.mn)) == 0,
                "Identify Model should be correct");

    nvme_ctrl_cleanup(&ctrl);
    TEST_ASSERT(true, "nvme_ctrl_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(nvme_ctrl_init(NULL) == HFSSS_ERR_INVAL, "nvme_ctrl_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* MSI-X Tests */
static int test_msix(void)
{
    printf("\n=== MSI-X Tests ===\n");

    struct msix_ctx ctx;
    u64 val;
    int ret;

    ret = msix_init(&ctx, 32);
    TEST_ASSERT(ret == HFSSS_OK, "msix_init should succeed");

    ret = msix_enable(&ctx);
    TEST_ASSERT(ret == HFSSS_OK, "msix_enable should succeed");

    /* Write and read MSI-X Table */
    ret = msix_table_write(&ctx, 0, 0x12345678, 4);
    TEST_ASSERT(ret == HFSSS_OK, "msix_table_write should succeed");
    ret = msix_table_read(&ctx, 0, &val, 4);
    TEST_ASSERT(ret == HFSSS_OK, "msix_table_read should succeed");

    msix_disable(&ctx);
    msix_cleanup(&ctx);
    TEST_ASSERT(true, "msix_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(msix_init(NULL, 32) == HFSSS_ERR_INVAL, "msix_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* DMA Tests */
static int test_dma(void)
{
    printf("\n=== DMA Tests ===\n");

    struct dma_ctx ctx;
    u8 test_data[4096];
    u8 read_data[4096];
    int ret;

    ret = dma_init(&ctx, 1024 * 1024);
    TEST_ASSERT(ret == HFSSS_OK, "dma_init should succeed");

    memset(test_data, 0xAA, sizeof(test_data));
    ret = dma_copy_to_prp(&ctx, 0x10000000, 0x20000000, test_data, 4096, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "dma_copy_to_prp should succeed");

    memset(read_data, 0, sizeof(read_data));
    ret = dma_copy_from_prp(&ctx, read_data, 0x10000000, 0x20000000, 4096, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "dma_copy_from_prp should succeed");

    dma_cleanup(&ctx);
    TEST_ASSERT(true, "dma_cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(dma_init(NULL, 1024) == HFSSS_ERR_INVAL, "dma_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* PCIe NVMe Device Tests */
static int test_pcie_nvme_dev(void)
{
    printf("\n=== PCIe NVMe Device Tests ===\n");

    struct pcie_nvme_dev dev;
    struct pcie_nvme_dev default_dev;
    struct pcie_nvme_dev uninit_dev;
    struct pcie_nvme_config config;
    u32 val32;
    u64 val64;
    char shmem_path[64];
    struct nvme_sq_entry cmd;
    struct nvme_cq_entry cpl;
    int ret;

    pcie_nvme_config_default(NULL);
    pcie_nvme_config_default(&config);
    snprintf(shmem_path, sizeof(shmem_path), "/hfsss_pcie_%ld", (long)getpid());
    shm_unlink(shmem_path);
    config.shmem_path = shmem_path;
    TEST_ASSERT(true, "pcie_nvme_config_default should succeed");
    TEST_ASSERT(config.max_queue_pairs == 64, "pcie_nvme default max queue pairs");
    TEST_ASSERT(config.namespace_count == 1, "pcie_nvme default namespace count");
    TEST_ASSERT(config.page_size == 4096, "pcie_nvme default page size");

    ret = pcie_nvme_dev_init(&default_dev, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_dev_init uses default config");
    pcie_nvme_dev_cleanup(&default_dev);

    ret = pcie_nvme_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_dev_init should succeed");

    /* Test PCI Config Read */
    ret = pcie_nvme_cfg_read(&dev, 0x00, &val32, 2);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_cfg_read should succeed");
    TEST_ASSERT(val32 == HFSSS_VENDOR_ID, "pcie_nvme_cfg_read returns vendor ID");

    ret = pcie_nvme_cfg_write(&dev, 0x04, PCI_CMD_MEM_EN | PCI_CMD_BUS_MASTER, 2);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_cfg_write command succeeds");

    memset(&uninit_dev, 0, sizeof(uninit_dev));
    TEST_ASSERT(pcie_nvme_cfg_read(NULL, 0, &val32, 2) == HFSSS_ERR_INVAL,
                "pcie_nvme_cfg_read rejects NULL dev");
    TEST_ASSERT(pcie_nvme_cfg_read(&uninit_dev, 0, &val32, 2) == HFSSS_ERR_INVAL,
                "pcie_nvme_cfg_read rejects uninitialized dev");
    TEST_ASSERT(pcie_nvme_cfg_write(NULL, 0, 0, 2) == HFSSS_ERR_INVAL,
                "pcie_nvme_cfg_write rejects NULL dev");
    TEST_ASSERT(pcie_nvme_cfg_write(&uninit_dev, 0, 0, 2) == HFSSS_ERR_INVAL,
                "pcie_nvme_cfg_write rejects uninitialized dev");

    /* Test NVMe Register Read */
    ret = pcie_nvme_bar_read(&dev, 0, NVME_REG_CAP, &val64, 8);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_read CAP should succeed");
    TEST_ASSERT((val64 & NVME_CAP_MQES_MASK) != 0, "pcie_nvme_bar_read CAP has MQES");

    ret = pcie_nvme_bar_write(&dev, 0, NVME_REG_AQA, 0x00100010, 4);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_write BAR0 succeeds");
    ret = pcie_nvme_bar_read(&dev, 0, NVME_REG_AQA, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0x00100010,
                "pcie_nvme BAR0 read reflects register write");

    ret = pcie_nvme_bar_write(&dev, 2, 0, 0x12345678, 4);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_write BAR2 MSI-X table succeeds");
    ret = pcie_nvme_bar_read(&dev, 2, 0, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK && (u32)val64 == 0x12345678,
                "pcie_nvme_bar_read BAR2 MSI-X table succeeds");

    ret = pcie_nvme_bar_read(&dev, 4, 0, &val64, 8);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_read BAR4 PBA succeeds");
    ret = pcie_nvme_bar_write(&dev, 4, 0, 0, 8);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_write BAR4 PBA noop succeeds");
    ret = pcie_nvme_bar_read(&dev, 5, 0, &val64, 4);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_read default PCI BAR succeeds");
    ret = pcie_nvme_bar_write(&dev, 5, 0, 0xAA, 4);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_write default PCI BAR succeeds");

    TEST_ASSERT(pcie_nvme_bar_read(NULL, 0, 0, &val64, 4) == HFSSS_ERR_INVAL,
                "pcie_nvme_bar_read rejects NULL dev");
    TEST_ASSERT(pcie_nvme_bar_read(&uninit_dev, 0, 0, &val64, 4) == HFSSS_ERR_INVAL,
                "pcie_nvme_bar_read rejects uninitialized dev");
    TEST_ASSERT(pcie_nvme_bar_write(NULL, 0, 0, 0, 4) == HFSSS_ERR_INVAL,
                "pcie_nvme_bar_write rejects NULL dev");
    TEST_ASSERT(pcie_nvme_bar_write(&uninit_dev, 0, 0, 0, 4) == HFSSS_ERR_INVAL,
                "pcie_nvme_bar_write rejects uninitialized dev");

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    ret = pcie_nvme_process_admin_cmd(&dev, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_process_admin_cmd delegates to controller");
    ret = pcie_nvme_process_io_cmd(&dev, &cmd, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_process_io_cmd delegates to controller");
    TEST_ASSERT(pcie_nvme_process_admin_cmd(NULL, &cmd, &cpl) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_admin_cmd rejects NULL dev");
    TEST_ASSERT(pcie_nvme_process_admin_cmd(&uninit_dev, &cmd, &cpl) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_admin_cmd rejects uninitialized dev");
    TEST_ASSERT(pcie_nvme_process_admin_cmd(&dev, NULL, &cpl) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_admin_cmd rejects NULL cmd");
    TEST_ASSERT(pcie_nvme_process_admin_cmd(&dev, &cmd, NULL) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_admin_cmd rejects NULL cpl");
    TEST_ASSERT(pcie_nvme_process_io_cmd(NULL, &cmd, &cpl) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_io_cmd rejects NULL dev");
    TEST_ASSERT(pcie_nvme_process_io_cmd(&uninit_dev, &cmd, &cpl) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_io_cmd rejects uninitialized dev");
    TEST_ASSERT(pcie_nvme_process_io_cmd(&dev, NULL, &cpl) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_io_cmd rejects NULL cmd");
    TEST_ASSERT(pcie_nvme_process_io_cmd(&dev, &cmd, NULL) == HFSSS_ERR_INVAL,
                "pcie_nvme_process_io_cmd rejects NULL cpl");

    pcie_nvme_dev_stop(NULL);

    ret = pcie_nvme_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_dev_start should succeed");
    TEST_ASSERT(dev.running == true, "pcie_nvme_dev_start marks running");
    ret = pcie_nvme_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_dev_start is idempotent");
    TEST_ASSERT(pcie_nvme_dev_start(NULL) == HFSSS_ERR_INVAL,
                "pcie_nvme_dev_start rejects NULL dev");
    TEST_ASSERT(pcie_nvme_dev_start(&uninit_dev) == HFSSS_ERR_INVAL,
                "pcie_nvme_dev_start rejects uninitialized dev");

    pcie_nvme_dev_stop(&dev);
    TEST_ASSERT(dev.running == false, "pcie_nvme_dev_stop clears running");
    pcie_nvme_dev_stop(&dev);
    TEST_ASSERT(dev.running == false, "pcie_nvme_dev_stop is idempotent");

    pcie_nvme_dev_cleanup(&dev);
    TEST_ASSERT(true, "pcie_nvme_dev_cleanup should succeed");
    pcie_nvme_dev_cleanup(NULL);
    shm_unlink(shmem_path);

    /* Test NULL handling */
    TEST_ASSERT(pcie_nvme_dev_init(NULL, &config) == HFSSS_ERR_INVAL, "pcie_nvme_dev_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Queue Manager Tests */
static int test_queue_mgr(void)
{
    printf("\n=== Queue Manager Tests ===\n");

    struct nvme_queue_mgr mgr;
    struct nvme_sq sq;
    struct nvme_cq cq;
    struct prp_walker prp;
    struct sgl_walker sgl;
    int ret;

    ret = nvme_queue_mgr_init(&mgr, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_queue_mgr_init should succeed");

    ret = nvme_sq_create(&sq, 1, 0x10000000, 256, 64, 1);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_sq_create should succeed");

    ret = nvme_cq_create(&cq, 1, 0x20000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "nvme_cq_create should succeed");

    ret = prp_walker_init(&prp, 0x1000, 0x2000, 4096, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "prp_walker_init should succeed");

    ret = sgl_walker_init(&sgl, NULL, 0);
    TEST_ASSERT(ret == HFSSS_OK, "sgl_walker_init should succeed");

    nvme_sq_destroy(&sq);
    nvme_cq_destroy(&cq);
    nvme_queue_mgr_cleanup(&mgr);
    TEST_ASSERT(true, "queue cleanup should succeed");

    /* Test NULL handling */
    TEST_ASSERT(nvme_queue_mgr_init(NULL, NULL) == HFSSS_ERR_INVAL, "nvme_queue_mgr_init with NULL should fail");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_sq_prp_sgl_edges(void)
{
    printf("\n=== SQ / PRP / SGL Edge Tests ===\n");

    struct nvme_sq sq;
    struct nvme_sq_entry *entries;
    struct nvme_sq_entry out;
    int ret;

    TEST_ASSERT(nvme_sq_create(NULL, 1, 0, 4, 64, 1) == HFSSS_ERR_INVAL,
                "sq-edge: create NULL sq rejected");

    ret = nvme_sq_create(&sq, 1, 0x10000000, 4, 64, 1);
    TEST_ASSERT(ret == HFSSS_OK, "sq-edge: create qsize 4");
    TEST_ASSERT(nvme_sq_has_pending(&sq) == false,
                "sq-edge: fresh sq has no pending commands");
    TEST_ASSERT(nvme_sq_fetch_cmd(&sq, &out) == HFSSS_ERR_AGAIN,
                "sq-edge: fetch empty sq returns AGAIN");
    TEST_ASSERT(nvme_sq_update_tail(NULL, 0) == HFSSS_ERR_INVAL,
                "sq-edge: update_tail NULL rejected");
    TEST_ASSERT(nvme_sq_update_tail(&sq, 4) == HFSSS_ERR_INVAL,
                "sq-edge: update_tail out of range rejected");

    entries = (struct nvme_sq_entry *)sq.entries;
    memset(entries, 0, 4 * sizeof(*entries));
    entries[0].opcode = NVME_NVM_READ;
    entries[0].command_id = 10;
    entries[1].opcode = NVME_NVM_WRITE;
    entries[1].command_id = 11;
    ret = nvme_sq_update_tail(&sq, 2);
    TEST_ASSERT(ret == HFSSS_OK && sq.doorbell_writes == 1,
                "sq-edge: update_tail advances doorbell count");
    TEST_ASSERT(nvme_sq_has_pending(&sq) == true,
                "sq-edge: pending commands visible after tail update");
    ret = nvme_sq_fetch_cmd(&sq, &out);
    TEST_ASSERT(ret == HFSSS_OK && out.command_id == 10 && sq.sq_head == 1,
                "sq-edge: fetch first command advances head");
    ret = nvme_sq_fetch_cmd(&sq, &out);
    TEST_ASSERT(ret == HFSSS_OK && out.command_id == 11 && sq.sq_head == 2,
                "sq-edge: fetch second command advances head");
    TEST_ASSERT(nvme_sq_has_pending(&sq) == false,
                "sq-edge: sq empty after two fetches");

    entries[3].opcode = NVME_NVM_FLUSH;
    entries[3].command_id = 13;
    entries[0].opcode = NVME_NVM_WRITE_ZEROES;
    entries[0].command_id = 14;
    sq.sq_head = 3;
    sq.sq_tail = 1;
    ret = nvme_sq_fetch_cmd(&sq, &out);
    TEST_ASSERT(ret == HFSSS_OK && out.command_id == 13 && sq.sq_head == 0,
                "sq-edge: fetch wraps head from last slot to zero");
    ret = nvme_sq_fetch_cmd(&sq, &out);
    TEST_ASSERT(ret == HFSSS_OK && out.command_id == 14 && sq.sq_head == 1,
                "sq-edge: fetch reads wrapped slot zero");

    struct nvme_sq disabled_sq;
    memset(&disabled_sq, 0, sizeof(disabled_sq));
    TEST_ASSERT(nvme_sq_fetch_cmd(&disabled_sq, &out) == HFSSS_ERR_INVAL,
                "sq-edge: fetch disabled sq rejected");
    TEST_ASSERT(nvme_sq_has_pending(&disabled_sq) == false,
                "sq-edge: disabled sq has no pending commands");
    nvme_sq_destroy(&sq);
    nvme_sq_destroy(NULL);

    struct prp_walker prp;
    u64 addr;
    u32 len;
    TEST_ASSERT(prp_walker_init(NULL, 0, 0, 0, 4096) == HFSSS_ERR_INVAL,
                "prp-edge: init NULL rejected");
    /* 2-page transfer: PRP2 is a direct data page */
    ret = prp_walker_init(&prp, 0x1800, 0x4000, 2048 + 4096, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "prp-edge: init two pages");
    TEST_ASSERT(prp.total_pages == 2, "prp-edge: detected 2-page transfer");
    TEST_ASSERT(!prp.use_prp_list, "prp-edge: PRP2 is direct page (not list)");
    ret = prp_walker_next(&prp, &addr, &len);
    TEST_ASSERT(ret == HFSSS_OK && addr == 0x1800 && len == 2048,
                "prp-edge: first PRP honors page offset");
    ret = prp_walker_next(&prp, &addr, &len);
    TEST_ASSERT(ret == HFSSS_OK && addr == 0x4000 && len == 4096,
                "prp-edge: second page uses prp2 as direct data page");
    TEST_ASSERT(prp_walker_next(&prp, &addr, &len) == HFSSS_ERR_NOENT,
                "prp-edge: exhausted after two pages");
    TEST_ASSERT(prp_walker_next(NULL, &addr, &len) == HFSSS_ERR_INVAL,
                "prp-edge: next NULL walker rejected");

    struct sgl_walker sgl;
    struct sgl_segment *sgl_buf = calloc(2, sizeof(struct sgl_segment));
    sgl_buf[0].type = SGL_DESC_TYPE_DATA;
    sgl_buf[0].subtype = SGL_SUBTYPE_ADDRESS;
    sgl_buf[0].length = 0;
    sgl_buf[0].address = 0;
    TEST_ASSERT(sgl_walker_init(NULL, NULL, 0) == HFSSS_ERR_INVAL,
                "sgl-edge: init NULL rejected");
    TEST_ASSERT(sgl_walker_init(&sgl, sgl_buf, 2 * sizeof(struct sgl_segment)) == HFSSS_OK,
                "sgl-edge: init succeeds");
    TEST_ASSERT(sgl_walker_next(&sgl, &addr, &len) == HFSSS_ERR_NOENT,
                "sgl-edge: next reports NOENT on zero-length descriptors");
    TEST_ASSERT(sgl_walker_next(NULL, &addr, &len) == HFSSS_ERR_INVAL,
                "sgl-edge: next NULL rejected");
    TEST_ASSERT(sgl_walker_skip(&sgl, 16) == HFSSS_ERR_INVAL,
                "sgl-edge: skip reports INVAL on zero-length descriptors");
    TEST_ASSERT(sgl_walker_skip(NULL, 16) == HFSSS_ERR_INVAL,
                "sgl-edge: skip NULL rejected");
    free(sgl_buf);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Shared Memory Tests (simplified) */
static int test_shmem(void)
{
    printf("\n=== Shared Memory Tests ===\n");

    struct shmem_ctx producer;
    struct shmem_ctx consumer;
    struct shmem_ctx bad_open;
    struct shmem_cmd_slot slot;
    struct shmem_cmd_slot out;
    struct nvme_sq_entry sqe;
    struct nvme_sq_entry sqe_out;
    char path[64];
    int ret;

    snprintf(path, sizeof(path), "/hfsss_shmem_%ld", (long)getpid());
    shm_unlink(path);

    TEST_ASSERT(shmem_create(NULL, path) == HFSSS_ERR_INVAL,
                "shmem_create rejects NULL ctx");
    TEST_ASSERT(shmem_create(&producer, NULL) == HFSSS_ERR_INVAL,
                "shmem_create rejects NULL path");
    TEST_ASSERT(shmem_open(NULL, path) == HFSSS_ERR_INVAL,
                "shmem_open rejects NULL ctx");
    TEST_ASSERT(shmem_open(&consumer, NULL) == HFSSS_ERR_INVAL,
                "shmem_open rejects NULL path");

    ret = shmem_create(&producer, path);
    TEST_ASSERT(ret == HFSSS_OK, "shmem_create succeeds");
    TEST_ASSERT(producer.layout->header.magic == RING_BUFFER_MAGIC,
                "shmem_create initializes ring magic");
    TEST_ASSERT(producer.layout->header.slot_count == RING_BUFFER_SLOTS,
                "shmem_create initializes slot count");

    ret = shmem_open(&consumer, path);
    TEST_ASSERT(ret == HFSSS_OK, "shmem_open succeeds");
    TEST_ASSERT(consumer.is_producer == false, "shmem_open creates consumer role");

    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 17;
    slot.type = SHMEM_CMD_NVME_IO;
    slot.status = 0x1234;

    TEST_ASSERT(shmem_produce_cmd(NULL, &slot) == HFSSS_ERR_INVAL,
                "shmem_produce_cmd rejects NULL ctx");
    TEST_ASSERT(shmem_produce_cmd(&producer, NULL) == HFSSS_ERR_INVAL,
                "shmem_produce_cmd rejects NULL slot");
    TEST_ASSERT(shmem_produce_cmd(&consumer, &slot) == HFSSS_ERR_INVAL,
                "shmem_produce_cmd rejects non-producer ctx");

    ret = shmem_produce_cmd(&producer, &slot);
    TEST_ASSERT(ret == HFSSS_OK, "shmem_produce_cmd succeeds");
    TEST_ASSERT(producer.sent_count == 1, "shmem producer sent count increments");
    TEST_ASSERT(producer.layout->header.total_produced == 1,
                "shmem total produced increments");

    TEST_ASSERT(shmem_consume_cmd(NULL, &out) == HFSSS_ERR_INVAL,
                "shmem_consume_cmd rejects NULL ctx");
    TEST_ASSERT(shmem_consume_cmd(&consumer, NULL) == HFSSS_ERR_INVAL,
                "shmem_consume_cmd rejects NULL output slot");
    producer.is_consumer = false;
    TEST_ASSERT(shmem_consume_cmd(&producer, &out) == HFSSS_ERR_INVAL,
                "shmem_consume_cmd rejects non-consumer ctx");
    producer.is_consumer = true;

    memset(&out, 0, sizeof(out));
    ret = shmem_consume_cmd(&consumer, &out);
    TEST_ASSERT(ret == HFSSS_OK, "shmem_consume_cmd succeeds");
    TEST_ASSERT(out.slot_id == slot.slot_id && out.status == slot.status,
                "shmem consume returns produced slot");
    TEST_ASSERT(consumer.recv_count == 1, "shmem consumer recv count increments");
    TEST_ASSERT(consumer.layout->header.total_consumed == 1,
                "shmem total consumed increments");
    TEST_ASSERT(shmem_consume_cmd(&consumer, &out) == HFSSS_ERR_NOENT,
                "shmem_consume_cmd reports empty ring");

    for (u32 i = 0; i < RING_BUFFER_SLOTS - 1; i++) {
        slot.slot_id = i;
        ret = shmem_produce_cmd(&producer, &slot);
        if (ret != HFSSS_OK) {
            break;
        }
    }
    TEST_ASSERT(ret == HFSSS_OK, "shmem ring fills to one free sentinel slot");
    TEST_ASSERT(shmem_produce_cmd(&producer, &slot) == HFSSS_ERR_NOSPC,
                "shmem_produce_cmd reports full ring");
    TEST_ASSERT(producer.layout->header.overflow_count == 1,
                "shmem overflow count increments");

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_NVM_WRITE;
    sqe.command_id = 0x55AA;
    memset(&slot, 0xA5, sizeof(slot));
    shmem_nvme_to_slot(NULL, &sqe, 1);
    shmem_nvme_to_slot(&slot, NULL, 1);
    shmem_nvme_to_slot(&slot, &sqe, 0);
    TEST_ASSERT(slot.type == SHMEM_CMD_NVME_ADMIN,
                "shmem_nvme_to_slot maps qid 0 to admin");
    TEST_ASSERT(slot.cmd_len == sizeof(struct nvme_sq_entry),
                "shmem_nvme_to_slot stores command length");
    TEST_ASSERT(slot.nvme_cmd.command_id == sqe.command_id,
                "shmem_nvme_to_slot copies command");
    shmem_slot_to_nvme(NULL, &slot);
    shmem_slot_to_nvme(&sqe_out, NULL);
    memset(&sqe_out, 0, sizeof(sqe_out));
    shmem_slot_to_nvme(&sqe_out, &slot);
    TEST_ASSERT(sqe_out.command_id == sqe.command_id,
                "shmem_slot_to_nvme copies command back");

    producer.layout->header.magic = 0;
    ret = shmem_open(&bad_open, path);
    TEST_ASSERT(ret == HFSSS_ERR, "shmem_open rejects bad magic");
    producer.layout->header.magic = RING_BUFFER_MAGIC;

    shmem_close(NULL);
    shmem_close(&consumer);
    shmem_close(&producer);
    shm_unlink(path);

    return TEST_PASS;
}

/* CQ Head Update Tests */
static int test_cq_update_head(void)
{
    printf("\n=== CQ Head Update Tests ===\n");

    struct nvme_cq cq;
    int ret;

    ret = nvme_cq_create(&cq, 1, 0x20000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "cq_create for head update test");

    ret = nvme_cq_update_head(&cq, 10);
    TEST_ASSERT(ret == HFSSS_OK, "update_head to 10 should succeed");
    TEST_ASSERT(cq.cq_head == 10, "cq_head should be 10 after update");

    ret = nvme_cq_update_head(&cq, 255);
    TEST_ASSERT(ret == HFSSS_OK, "update_head to last valid index");
    TEST_ASSERT(cq.cq_head == 255, "cq_head should be 255");

    ret = nvme_cq_update_head(&cq, 256);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "update_head beyond qsize rejected");

    ret = nvme_cq_update_head(NULL, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "update_head with NULL cq rejected");

    struct nvme_cq disabled_cq;
    memset(&disabled_cq, 0, sizeof(disabled_cq));
    ret = nvme_cq_update_head(&disabled_cq, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "update_head on disabled cq rejected");

    nvme_cq_destroy(&cq);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* CQ Post Completion Tests */
static int test_cq_post_cpl(void)
{
    printf("\n=== CQ Post Completion Tests ===\n");

    struct nvme_cq cq;
    struct nvme_cq_entry cpl;
    int ret;

    ret = nvme_cq_create(&cq, 1, 0x20000000, 4, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "cq_create for post_cpl test");
    TEST_ASSERT(cq.phase == 1, "initial phase should be 1");

    memset(&cpl, 0, sizeof(cpl));
    cpl.sq_id = 1;
    cpl.command_id = 42;
    cpl.status = NVME_BUILD_STATUS(NVME_SC_SUCCESS, NVME_STATUS_TYPE_GENERIC);

    ret = nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(ret == HFSSS_OK, "post_cpl should succeed");
    TEST_ASSERT(cq.cq_tail == 1, "cq_tail should advance to 1");
    TEST_ASSERT(cq.cpl_count == 1, "cpl_count should be 1");
    TEST_ASSERT(cq.interrupt_count == 1, "interrupt_count incremented");
    TEST_ASSERT((cpl.status & NVME_STATUS_PHASE_BIT) == 1, "phase bit stamped in completion status");

    ret = nvme_cq_post_cpl(NULL, &cpl);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "post_cpl with NULL cq rejected");

    ret = nvme_cq_post_cpl(&cq, NULL);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "post_cpl with NULL cpl rejected");

    nvme_cq_destroy(&cq);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* CQ Needs Interrupt Tests */
static int test_cq_needs_interrupt(void)
{
    printf("\n=== CQ Needs Interrupt Tests ===\n");

    struct nvme_cq cq;
    struct nvme_cq_entry cpl;
    int ret;

    ret = nvme_cq_create(&cq, 1, 0x20000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "cq_create for needs_interrupt test");

    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == false, "no interrupt when head == tail");

    memset(&cpl, 0, sizeof(cpl));
    nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == true, "interrupt needed after post with intr enabled");

    TEST_ASSERT(nvme_cq_needs_interrupt(NULL) == false, "needs_interrupt with NULL returns false");

    nvme_cq_destroy(&cq);

    /* Interrupt-disabled CQ */
    ret = nvme_cq_create(&cq, 2, 0x30000000, 256, 16, false);
    TEST_ASSERT(ret == HFSSS_OK, "cq_create with interrupt disabled");

    memset(&cpl, 0, sizeof(cpl));
    nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == false, "no interrupt when intr disabled even with pending");

    nvme_cq_destroy(&cq);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Duplicate Create Failure Tests */
static int test_duplicate_create_failure(void)
{
    printf("\n=== Duplicate Create Failure Tests ===\n");

    struct nvme_queue_mgr mgr;
    int ret;

    ret = nvme_queue_mgr_init(&mgr, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "mgr init for duplicate test");

    ret = nvme_create_io_cq(&mgr, 1, 0x20000000, 256, true);
    TEST_ASSERT(ret == HFSSS_OK, "first io_cq create succeeds");

    ret = nvme_create_io_cq(&mgr, 1, 0x20000000, 256, true);
    TEST_ASSERT(ret == HFSSS_ERR_EXIST, "duplicate io_cq create rejected");

    ret = nvme_create_io_sq(&mgr, 1, 0x10000000, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_OK, "first io_sq create succeeds");

    ret = nvme_create_io_sq(&mgr, 1, 0x10000000, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_ERR_EXIST, "duplicate io_sq create rejected");

    nvme_delete_io_sq(&mgr, 1);
    nvme_delete_io_cq(&mgr, 1);
    nvme_queue_mgr_cleanup(&mgr);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Invalid QID and Size Tests */
static int test_invalid_qid_and_size(void)
{
    printf("\n=== Invalid QID and Size Tests ===\n");

    struct nvme_queue_mgr mgr;
    int ret;

    ret = nvme_queue_mgr_init(&mgr, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "mgr init for invalid qid test");

    ret = nvme_create_io_cq(&mgr, 0, 0x20000000, 256, true);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "io_cq with qid 0 rejected");

    ret = nvme_create_io_cq(&mgr, MAX_QUEUE_PAIRS, 0x20000000, 256, true);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "io_cq with qid >= MAX rejected");

    ret = nvme_create_io_sq(&mgr, 0, 0x10000000, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "io_sq with qid 0 rejected");

    ret = nvme_create_io_sq(&mgr, MAX_QUEUE_PAIRS, 0x10000000, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "io_sq with qid >= MAX rejected");

    ret = nvme_create_io_sq(&mgr, 1, 0x10000000, 256, 5, 0);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "io_sq referencing absent cq rejected");

    ret = nvme_create_io_cq(NULL, 1, 0x20000000, 256, true);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "io_cq with NULL mgr rejected");

    ret = nvme_create_io_sq(NULL, 1, 0x10000000, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_ERR_INVAL, "io_sq with NULL mgr rejected");

    nvme_queue_mgr_cleanup(&mgr);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Delete-Busy and Delete-Clean Sequencing Tests */
static int test_delete_busy_and_clean(void)
{
    printf("\n=== Delete-Busy and Delete-Clean Tests ===\n");

    struct nvme_queue_mgr mgr;
    int ret;

    ret = nvme_queue_mgr_init(&mgr, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "mgr init for delete sequencing test");

    ret = nvme_create_io_cq(&mgr, 1, 0x20000000, 256, true);
    TEST_ASSERT(ret == HFSSS_OK, "create cq for delete test");

    ret = nvme_create_io_sq(&mgr, 1, 0x10000000, 256, 1, 0);
    TEST_ASSERT(ret == HFSSS_OK, "create sq associated with cq 1");

    ret = nvme_delete_io_cq(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_ERR_BUSY, "delete cq with associated sq rejected");

    ret = nvme_delete_io_sq(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_OK, "delete sq should succeed");

    ret = nvme_delete_io_cq(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_OK, "delete cq after sq removed succeeds");

    ret = nvme_delete_io_cq(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "delete already-deleted cq rejected");

    ret = nvme_delete_io_sq(&mgr, 1);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "delete already-deleted sq rejected");

    nvme_queue_mgr_cleanup(&mgr);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* PRP Walker Correctness Tests */
/* Interrupt Coalescing Tests */
static int test_intr_coalescing(void)
{
    printf("\n=== Interrupt Coalescing Tests ===\n");

    struct nvme_cq cq;
    struct nvme_cq_entry cpl;
    int ret;

    /* Test 1: Disabled coalescing (default: time=0, threshold=0) */
    ret = nvme_cq_create(&cq, 1, 0x20000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "coalesce: cq_create OK");
    TEST_ASSERT(cq.coalesce_time_us == 0 && cq.coalesce_threshold == 0,
                "coalesce: defaults to disabled (time=0, threshold=0)");

    memset(&cpl, 0, sizeof(cpl));
    nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == true,
                "coalesce: immediate interrupt when coalescing disabled");
    nvme_cq_destroy(&cq);

    /* Test 2: Threshold-based coalescing */
    ret = nvme_cq_create(&cq, 2, 0x30000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "coalesce: cq2 create OK");

    cq.coalesce_threshold = 3;
    cq.pending_completions = 0;

    nvme_cq_post_cpl(&cq, &cpl);
    nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(cq.pending_completions == 2,
                "coalesce: pending_completions == 2 after 2 posts");
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == false,
                "coalesce: no interrupt below threshold (2 < 3)");

    nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(cq.pending_completions == 3,
                "coalesce: pending_completions == 3 after 3rd post");
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == true,
                "coalesce: interrupt fires at threshold (3 >= 3)");
    TEST_ASSERT(cq.pending_completions == 0,
                "coalesce: pending_completions reset after interrupt");
    nvme_cq_destroy(&cq);

    /* Test 3: Time-based coalescing */
    ret = nvme_cq_create(&cq, 3, 0x40000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "coalesce: cq3 create OK");

    cq.coalesce_time_us = 1000000; /* 1 second window */
    cq.pending_completions = 0;
    cq.last_interrupt_ts_ns = get_time_ns(); /* start timer now */

    nvme_cq_post_cpl(&cq, &cpl);
    /* Immediately check — should be within the 1s window */
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == false,
                "coalesce: no interrupt within time window");
    /* Manually expire the window by resetting last_interrupt_ts_ns to long ago */
    cq.last_interrupt_ts_ns = get_time_ns() - 2000000000ULL; /* 2 seconds ago */
    nvme_cq_post_cpl(&cq, &cpl);
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == true,
                "coalesce: interrupt fires after time window expires");
    nvme_cq_destroy(&cq);

    /* Test 4: Combined threshold + time — threshold fires first */
    ret = nvme_cq_create(&cq, 4, 0x50000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "coalesce: cq4 create OK");

    cq.coalesce_time_us = 1000000;
    cq.coalesce_threshold = 2;
    cq.pending_completions = 0;

    nvme_cq_post_cpl(&cq, &cpl);
    /* 1 < 2 threshold, still within time window */
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == false,
                "coalesce: combined — no interrupt below threshold");
    nvme_cq_post_cpl(&cq, &cpl);
    /* 2 >= 2 threshold triggers immediately regardless of time */
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq) == true,
                "coalesce: combined — threshold fires despite time window");
    nvme_cq_destroy(&cq);

    /* Test 5: per-CQ independence */
    struct nvme_cq cq_a, cq_b;
    ret = nvme_cq_create(&cq_a, 5, 0x60000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "coalesce: cq_a create OK");
    ret = nvme_cq_create(&cq_b, 6, 0x70000000, 256, 16, true);
    TEST_ASSERT(ret == HFSSS_OK, "coalesce: cq_b create OK");

    cq_a.coalesce_threshold = 2;
    cq_b.coalesce_threshold = 500; /* very high, won't fire */

    nvme_cq_post_cpl(&cq_a, &cpl);
    nvme_cq_post_cpl(&cq_b, &cpl);

    TEST_ASSERT(nvme_cq_needs_interrupt(&cq_a) == false,
                "coalesce: cq_a not at threshold yet (1 < 2)");
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq_b) == false,
                "coalesce: cq_b not at threshold yet (1 < 500)");

    nvme_cq_post_cpl(&cq_a, &cpl);
    TEST_ASSERT(nvme_cq_needs_interrupt(&cq_a) == true,
                "coalesce: cq_a fires at threshold, independent of cq_b");
    TEST_ASSERT(cq_b.pending_completions == 1,
                "coalesce: cq_b pending count unaffected by cq_a");

    nvme_cq_destroy(&cq_a);
    nvme_cq_destroy(&cq_b);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* SGL Walker Tests */
static int test_sgl_walker(void)
{
    printf("\n=== SGL Walker Tests ===\n");

    struct sgl_walker sgl;
    struct sgl_segment *seg;
    u64 addr;
    u32 len;
    int ret;

    /* Test 1: Single Data Block descriptor */
    {
        seg = calloc(1, sizeof(*seg));
        seg->type = SGL_DESC_TYPE_DATA;
        seg->subtype = SGL_SUBTYPE_ADDRESS;
        seg->length = 512;
        seg->address = 0xABCD0000;

        ret = sgl_walker_init(&sgl, seg, sizeof(*seg));
        TEST_ASSERT(ret == HFSSS_OK, "sgl: init single descriptor OK");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK, "sgl: next returns OK");
        TEST_ASSERT(addr == 0xABCD0000 && len == 512,
                    "sgl: data block addr==desc->address, len==desc->length");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_ERR_NOENT,
                    "sgl: NOENT after single descriptor exhausted");
        free(seg);
    }

    /* Test 2: Bit Bucket descriptor — returns sentinel address, skip semantics */
    {
        seg = calloc(1, sizeof(*seg));
        seg->type = SGL_DESC_TYPE_BIT;
        seg->subtype = SGL_SUBTYPE_ADDRESS;
        seg->length = 256;
        seg->address = 0xDEAD0000; /* ignored */

        ret = sgl_walker_init(&sgl, seg, sizeof(*seg));
        TEST_ASSERT(ret == HFSSS_OK, "sgl: init bit bucket OK");
        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK, "sgl: bit bucket next returns OK");
        TEST_ASSERT(addr == (u64)SGL_BIT_BUCKET_SENTINEL && len == 256,
                    "sgl: bit bucket returns sentinel addr and correct len");
        free(seg);
    }

    /* Test 3: Multiple descriptors in one segment */
    {
        seg = calloc(3, sizeof(*seg));
        seg[0].type = SGL_DESC_TYPE_DATA;
        seg[0].subtype = SGL_SUBTYPE_ADDRESS;
        seg[0].length = 100;
        seg[0].address = 0x1000;

        seg[1].type = SGL_DESC_TYPE_DATA;
        seg[1].subtype = SGL_SUBTYPE_ADDRESS;
        seg[1].length = 200;
        seg[1].address = 0x2000;

        seg[2].type = SGL_DESC_TYPE_DATA;
        seg[2].subtype = SGL_SUBTYPE_ADDRESS;
        seg[2].length = 300;
        seg[2].address = 0x3000;

        ret = sgl_walker_init(&sgl, seg, 3 * sizeof(*seg));
        TEST_ASSERT(ret == HFSSS_OK, "sgl: multi-desc init OK");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x1000 && len == 100,
                    "sgl: desc[0] = 0x1000 len=100");
        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x2000 && len == 200,
                    "sgl: desc[1] = 0x2000 len=200");
        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x3000 && len == 300,
                    "sgl: desc[2] = 0x3000 len=300");
        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_ERR_NOENT,
                    "sgl: NOENT after all descriptors");
        free(seg);
    }

    /* Test 4: Segment chain — Segment descriptor points to next segment */
    {
        struct sgl_segment *seg1 = calloc(2, sizeof(*seg1));
        struct sgl_segment *seg2 = calloc(1, sizeof(*seg2));

        seg1[0].type = SGL_DESC_TYPE_SEG;
        seg1[0].subtype = SGL_SUBTYPE_ADDRESS;
        seg1[0].length = sizeof(*seg2);  /* size of the next segment */
        seg1[0].address = (u64)(uintptr_t)seg2;

        seg1[1].type = SGL_DESC_TYPE_DATA;
        seg1[1].subtype = SGL_SUBTYPE_ADDRESS;
        seg1[1].length = 64;
        seg1[1].address = 0xF000;

        seg2[0].type = SGL_DESC_TYPE_DATA;
        seg2[0].subtype = SGL_SUBTYPE_ADDRESS;
        seg2[0].length = 128;
        seg2[0].address = 0x11000;

        ret = sgl_walker_init(&sgl, seg1, 2 * sizeof(*seg1));
        TEST_ASSERT(ret == HFSSS_OK, "sgl: segment chain init OK");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK, "sgl: chain step 1 OK");
        /* seg1[0] is Segment type — entered seg2. First data is seg2[0] (=0x11000). */
        TEST_ASSERT(addr == 0x11000 && len == 128,
                    "sgl: chain — data desc in seg2 (entered via Segment)");

        /* Next: seg2 exhausted, return to seg1, process seg1[1] (=0xF000). */
        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK, "sgl: chain step 2 OK");
        TEST_ASSERT(addr == 0xF000 && len == 64,
                    "sgl: chain — data desc in seg1 after returning from seg2");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_ERR_NOENT,
                    "sgl: NOENT after chain");
        free(seg1);
        free(seg2);
    }

    /* Test 5: Last Segment terminates after its descriptors */
    {
        struct sgl_segment *seg1 = calloc(2, sizeof(*seg1));
        struct sgl_segment *seg2 = calloc(1, sizeof(*seg2));

        seg1[0].type = SGL_DESC_TYPE_LAST;
        seg1[0].subtype = SGL_SUBTYPE_ADDRESS;
        seg1[0].length = sizeof(*seg2);
        seg1[0].address = (u64)(uintptr_t)seg2;

        seg2[0].type = SGL_DESC_TYPE_DATA;
        seg2[0].subtype = SGL_SUBTYPE_ADDRESS;
        seg2[0].length = 77;
        seg2[0].address = 0x9999;

        ret = sgl_walker_init(&sgl, seg1, sizeof(*seg1));
        TEST_ASSERT(ret == HFSSS_OK, "sgl: last-seg init OK");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x9999 && len == 77,
                    "sgl: last-seg — data in seg2");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_ERR_NOENT,
                    "sgl: last-seg NOENT after seg2 exhausted");
        free(seg1);
        free(seg2);
    }

    /* Test 6: sgl_walker_skip */
    {
        seg = calloc(2, sizeof(*seg));
        seg[0].type = SGL_DESC_TYPE_DATA;
        seg[0].subtype = SGL_SUBTYPE_ADDRESS;
        seg[0].length = 1000;
        seg[0].address = 0x5000;

        ret = sgl_walker_init(&sgl, seg, 2 * sizeof(*seg));
        TEST_ASSERT(ret == HFSSS_OK, "sgl: skip init OK");

        ret = sgl_walker_skip(&sgl, 600);
        TEST_ASSERT(ret == HFSSS_OK, "sgl: skip 600 OK");

        ret = sgl_walker_next(&sgl, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x5000 + 600,
                    "sgl: after skip, next addr is offset by skip amount");
        TEST_ASSERT(len == 400,
                    "sgl: after skip, remaining in descriptor = 1000-600");
        free(seg);
    }

    /* Test 7: NULL safety */
    TEST_ASSERT(sgl_walker_init(NULL, seg, sizeof(*seg)) == HFSSS_ERR_INVAL,
                "sgl: init NULL walker rejected");
    TEST_ASSERT(sgl_walker_next(NULL, &addr, &len) == HFSSS_ERR_INVAL,
                "sgl: next NULL walker rejected");
    TEST_ASSERT(sgl_walker_next(&sgl, NULL, &len) == HFSSS_ERR_INVAL,
                "sgl: next NULL addr rejected");
    TEST_ASSERT(sgl_walker_skip(NULL, 16) == HFSSS_ERR_INVAL,
                "sgl: skip NULL walker rejected");
    TEST_ASSERT(sgl_walker_skip(&sgl, 99999) == HFSSS_ERR_INVAL,
                "sgl: skip beyond remaining rejected");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_prp_walker_correctness(void)
{
    printf("\n=== PRP Walker Correctness Tests ===\n");

    struct prp_walker prp;
    u64 addr;
    u32 len;
    int ret;

    /* Test 1: Single page transfer */
    ret = prp_walker_init(&prp, 0x1800, 0x9999, 512, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "prp-walk: init single page OK");
    ret = prp_walker_next(&prp, &addr, &len);
    TEST_ASSERT(ret == HFSSS_OK && addr == 0x1800 && len == 512,
                "prp-walk: single page returns prp1 correct addr/len");
    TEST_ASSERT(prp_walker_next(&prp, &addr, &len) == HFSSS_ERR_NOENT,
                "prp-walk: single page exhausted");

    /* Test 2: Two-page transfer, PRP2 direct data page */
    ret = prp_walker_init(&prp, 0x1800, 0x5000, 3584, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "prp-walk: init two-page OK");
    ret = prp_walker_next(&prp, &addr, &len);
    TEST_ASSERT(ret == HFSSS_OK && addr == 0x1800 && len == 2048,
                "prp-walk: two-pg page0 honors prp1 offset");
    ret = prp_walker_next(&prp, &addr, &len);
    TEST_ASSERT(ret == HFSSS_OK && addr == 0x5000 && len == (3584 - 2048),
                "prp-walk: two-pg page1 = prp2 direct data page");
    TEST_ASSERT(prp_walker_next(&prp, &addr, &len) == HFSSS_ERR_NOENT,
                "prp-walk: two-page exhausted");

    /* Test 3: Three-page transfer, PRP2 is PRP list pointer */
    {
        u64 prp_list[2];
        prp_list[0] = 0x3000;
        prp_list[1] = 0x7000;

        ret = prp_walker_init(&prp, 0x1000, (u64)(uintptr_t)prp_list,
                              3 * 4096, 4096);
        TEST_ASSERT(ret == HFSSS_OK,
                    "prp-walk: init 3-page plist OK");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x1000 && len == 4096,
                    "prp-walk: 3pg page0 = prp1");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x3000 && len == 4096,
                    "prp-walk: 3pg page1 from prp list[0] (not prp2 as base)");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x7000 && len == 4096,
                    "prp-walk: 3pg page2 from prp list[1]");
        TEST_ASSERT(prp_walker_next(&prp, &addr, &len) == HFSSS_ERR_NOENT,
                    "prp-walk: 3-page exhausted");
    }

    /* Test 4: Five-page transfer with PRP list, non-contiguous pages */
    {
        u64 prp_list[4];
        prp_list[0] = 0xB000;
        prp_list[1] = 0xD000;
        prp_list[2] = 0xF000;
        prp_list[3] = 0x11000;

        ret = prp_walker_init(&prp, 0x9000, (u64)(uintptr_t)prp_list,
                              5 * 4096, 4096);
        TEST_ASSERT(ret == HFSSS_OK, "prp-walk: init 5-page plist OK");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x9000,
                    "prp-walk: 5pg page0 = prp1");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0xB000,
                    "prp-walk: 5pg page1 = plist[0]");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0xD000,
                    "prp-walk: 5pg page2 = plist[1]");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0xF000,
                    "prp-walk: 5pg page3 = plist[2]");
        ret = prp_walker_next(&prp, &addr, &len);
        TEST_ASSERT(ret == HFSSS_OK && addr == 0x11000,
                    "prp-walk: 5pg page4 = plist[3]");
        TEST_ASSERT(prp_walker_next(&prp, &addr, &len) == HFSSS_ERR_NOENT,
                    "prp-walk: 5-page exhausted");
    }

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    print_separator();
    printf("HFSSS PCIe/NVMe Emulation Module Tests\n");
    print_separator();

    test_pci_dev();
    test_nvme_ctrl();
    test_queue_mgr();
    test_sq_prp_sgl_edges();
    test_prp_walker_correctness();
    test_msix();
    test_dma();
    test_pcie_nvme_dev();
    test_shmem();
    test_cq_update_head();
    test_cq_post_cpl();
    test_cq_needs_interrupt();
    test_intr_coalescing();
    test_sgl_walker();
    test_duplicate_create_failure();
    test_invalid_qid_and_size();
    test_delete_busy_and_clean();

    print_separator();
    printf("Test Summary\n");
    print_separator();
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    print_separator();

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n  [FAILURE] Some tests failed!\n");
        return 1;
    }
}
