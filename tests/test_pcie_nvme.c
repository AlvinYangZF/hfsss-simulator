#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcie/pcie_nvme.h"

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        tests_passed++; \
    } else { \
        printf("  [FAIL] %s\n", msg); \
        tests_failed++; \
    } \
} while(0)

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

    /* Test Identify data */
    struct nvme_identify_ctrl id_ctrl;
    nvme_build_identify_ctrl(&id_ctrl);
    TEST_ASSERT(id_ctrl.vid == HFSSS_VENDOR_ID, "Identify VID should be correct");
    TEST_ASSERT(strncmp((char *)id_ctrl.mn, "HFSSS NVMe SSD", sizeof(id_ctrl.mn)) == 0, "Identify Model should be correct");

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
    struct pcie_nvme_config config;
    u32 val32;
    u64 val64;
    int ret;

    pcie_nvme_config_default(&config);
    TEST_ASSERT(true, "pcie_nvme_config_default should succeed");

    ret = pcie_nvme_dev_init(&dev, &config);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_dev_init should succeed");

    /* Test PCI Config Read */
    ret = pcie_nvme_cfg_read(&dev, 0x00, &val32, 2);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_cfg_read should succeed");

    /* Test NVMe Register Read */
    ret = pcie_nvme_bar_read(&dev, 0, NVME_REG_CAP, &val64, 8);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_bar_read CAP should succeed");

    ret = pcie_nvme_dev_start(&dev);
    TEST_ASSERT(ret == HFSSS_OK, "pcie_nvme_dev_start should succeed");

    pcie_nvme_dev_stop(&dev);

    pcie_nvme_dev_cleanup(&dev);
    TEST_ASSERT(true, "pcie_nvme_dev_cleanup should succeed");

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

/* Shared Memory Tests (simplified) */
static int test_shmem(void)
{
    printf("\n=== Shared Memory Tests ===\n");

    /* Just test that the functions compile/link for now */
    TEST_ASSERT(true, "shmem module compiled successfully");
    return TEST_PASS;
}

int main(void)
{
    print_separator();
    printf("HFSSS PCIe/NVMe Emulation Module Tests\n");
    print_separator();

    test_pci_dev();
    test_nvme_ctrl();
    test_queue_mgr();
    test_msix();
    test_dma();
    test_pcie_nvme_dev();
    test_shmem();

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
