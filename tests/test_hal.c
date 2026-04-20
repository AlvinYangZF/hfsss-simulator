#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "common/log.h"
#include "media/media.h"
#include "hal/hal.h"
#include "hal/hal_aer.h"
#include "hal/hal_pcie_link.h"

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

/* HAL NAND Device Tests */
static int test_hal_nand_dev(void)
{
    printf("\n=== HAL NAND Device Tests ===\n");

    struct hal_nand_dev dev;
    int ret;

    ret = hal_nand_dev_init(&dev, 2, 2, 2, 2, 10, 8, 4096, 64, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_dev_init should succeed");

    TEST_ASSERT(dev.channel_count == 2, "channel count should be 2");
    TEST_ASSERT(dev.chips_per_channel == 2, "chips per channel should be 2");
    TEST_ASSERT(dev.page_size == 4096, "page size should be 4096");

    hal_nand_dev_cleanup(&dev);

    /* Test NULL handling */
    TEST_ASSERT(hal_nand_dev_init(NULL, 2, 2, 2, 2, 10, 8, 4096, 64, NULL) == HFSSS_ERR_INVAL,
                "hal_nand_dev_init with NULL should fail");
    hal_nand_dev_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL NOR Tests */
static int test_hal_nor(void)
{
    printf("\n=== HAL NOR Tests ===\n");

    struct hal_nor_dev nor;
    int ret;
    u8 write_data[256];
    u8 read_data[256];

    ret = hal_nor_dev_init(&nor, 1024 * 1024, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_dev_init should succeed");
    TEST_ASSERT(nor.size == 1024 * 1024, "NOR size should be 1MB");

    /* Test NOR read - should read all 0xFF (erased state) */
    memset(read_data, 0, sizeof(read_data));
    ret = hal_nor_read(&nor, 0, read_data, sizeof(read_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read should succeed");
    bool all_ff = true;
    for (u32 i = 0; i < sizeof(read_data); i++) {
        if (read_data[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    TEST_ASSERT(all_ff, "NOR should be erased (all 0xFF) after init");

    /* Test NOR write - can only clear bits */
    memset(write_data, 0xAA, sizeof(write_data));
    ret = hal_nor_write(&nor, 0, write_data, sizeof(write_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_write should succeed");

    /* Read back and verify */
    memset(read_data, 0, sizeof(read_data));
    ret = hal_nor_read(&nor, 0, read_data, sizeof(read_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read should succeed");
    TEST_ASSERT(memcmp(read_data, write_data, sizeof(read_data)) == 0,
                "read data should match written data");

    /* Test NOR erase */
    ret = hal_nor_erase(&nor, 0, HAL_NOR_SECTOR_SIZE);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_erase should succeed");

    /* Verify erased */
    memset(read_data, 0, sizeof(read_data));
    ret = hal_nor_read(&nor, 0, read_data, sizeof(read_data));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read after erase should succeed");
    all_ff = true;
    for (u32 i = 0; i < sizeof(read_data); i++) {
        if (read_data[i] != 0xFF) {
            all_ff = false;
            break;
        }
    }
    TEST_ASSERT(all_ff, "NOR should be erased (all 0xFF) after erase");

    hal_nor_dev_cleanup(&nor);

    /* Test NULL handling */
    TEST_ASSERT(hal_nor_dev_init(NULL, 1024 * 1024, NULL) == HFSSS_ERR_INVAL,
                "hal_nor_dev_init with NULL should fail");
    hal_nor_dev_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL Power Tests */
static int test_hal_power(void)
{
    printf("\n=== HAL Power Tests ===\n");

    struct hal_power_ctx power;
    int ret;
    struct hal_power_state_desc desc;

    ret = hal_power_init(&power);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_init should succeed");

    TEST_ASSERT(hal_power_get_state(&power) == HAL_POWER_PS0, "initial state should be PS0");

    ret = hal_power_set_state(&power, HAL_POWER_PS2);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_set_state to PS2 should succeed");
    TEST_ASSERT(hal_power_get_state(&power) == HAL_POWER_PS2, "state should be PS2");

    /* Get state descriptor */
    ret = hal_power_get_state_desc(&power, HAL_POWER_PS0, &desc);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_get_state_desc should succeed");
    TEST_ASSERT(desc.non_operational == false, "PS0 should be operational");

    ret = hal_power_get_state_desc(&power, HAL_POWER_PS4, &desc);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_get_state_desc PS4 should succeed");
    TEST_ASSERT(desc.non_operational == true, "PS4 should be non-operational");

    hal_power_cleanup(&power);

    /* Test NULL handling */
    TEST_ASSERT(hal_power_init(NULL) == HFSSS_ERR_INVAL,
                "hal_power_init with NULL should fail");
    hal_power_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL PCI Tests */
static int test_hal_pci(void)
{
    printf("\n=== HAL PCI Tests ===\n");

    struct hal_pci_ctx pci;
    int ret;
    struct hal_pci_completion comp;
    struct hal_pci_namespace ns_info;
    u32 nsid_list[HAL_PCI_MAX_NAMESPACES];
    u32 count;

    ret = hal_pci_init(&pci);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_init should succeed");

    /* Test command completion submission (REQ-062) */
    memset(&comp, 0, sizeof(comp));
    comp.command_id = 0x1234;
    comp.status = 0;
    comp.result = 0x5678;
    ret = hal_pci_submit_completion(&pci, &comp);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_submit_completion should succeed");
    TEST_ASSERT(hal_pci_get_completion_count(&pci) == 1, "completion count should be 1");

    /* Test poll completion */
    memset(&comp, 0, sizeof(comp));
    ret = hal_pci_poll_completion(&pci, &comp);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_poll_completion should succeed");
    TEST_ASSERT(comp.command_id == 0x1234, "command ID should match");
    TEST_ASSERT(hal_pci_get_completion_count(&pci) == 0, "completion count should be 0 after poll");

    /* Test namespace management (REQ-065) */
    ret = hal_pci_ns_attach(&pci, 1, 100000, 512);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_attach NSID 1 should succeed");
    TEST_ASSERT(hal_pci_ns_get_active_count(&pci) == 1, "active NS count should be 1");

    ret = hal_pci_ns_attach(&pci, 2, 200000, 4096);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_attach NSID 2 should succeed");
    TEST_ASSERT(hal_pci_ns_get_active_count(&pci) == 2, "active NS count should be 2");

    /* Get NS info */
    ret = hal_pci_ns_get_info(&pci, 1, &ns_info);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_get_info NSID 1 should succeed");
    TEST_ASSERT(ns_info.nsid == 1, "NSID should be 1");
    TEST_ASSERT(ns_info.lba_size == 512, "LBA size should be 512");

    /* List NSIDs */
    count = HAL_PCI_MAX_NAMESPACES;
    ret = hal_pci_ns_list(&pci, nsid_list, &count);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_list should succeed");
    TEST_ASSERT(count == 2, "list count should be 2");

    /* Detach NS */
    ret = hal_pci_ns_detach(&pci, 1);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_ns_detach NSID 1 should succeed");
    TEST_ASSERT(hal_pci_ns_get_active_count(&pci) == 1, "active NS count should be 1 after detach");

    hal_pci_cleanup(&pci);

    /* Test NULL handling */
    TEST_ASSERT(hal_pci_init(NULL) == HFSSS_ERR_INVAL,
                "hal_pci_init with NULL should fail");
    hal_pci_cleanup(NULL);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HAL Tests */
static int test_hal(void)
{
    printf("\n=== HAL Module Tests ===\n");

    struct media_ctx media_ctx;
    struct media_config media_config;
    struct hal_nand_dev nand_dev;
    struct hal_nor_dev nor_dev;
    struct hal_pci_ctx pci_ctx;
    struct hal_power_ctx power_ctx;
    struct hal_ctx hal_ctx;
    int ret;

    /* Initialize media layer first */
    memset(&media_config, 0, sizeof(media_config));
    media_config.channel_count = 1;
    media_config.chips_per_channel = 1;
    media_config.dies_per_chip = 1;
    media_config.planes_per_die = 1;
    media_config.blocks_per_plane = 10;
    media_config.pages_per_block = 8;
    media_config.page_size = 4096;
    media_config.spare_size = 64;
    media_config.nand_type = NAND_TYPE_TLC;

    ret = media_init(&media_ctx, &media_config);
    TEST_ASSERT(ret == HFSSS_OK, "media_init should succeed");

    /* Initialize all HAL devices */
    ret = hal_nand_dev_init(&nand_dev, media_config.channel_count,
                           media_config.chips_per_channel,
                           media_config.dies_per_chip,
                           media_config.planes_per_die,
                           media_config.blocks_per_plane,
                           media_config.pages_per_block,
                           media_config.page_size,
                           media_config.spare_size,
                           &media_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_dev_init should succeed");

    ret = hal_nor_dev_init(&nor_dev, 1024 * 1024, NULL);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_dev_init should succeed");

    ret = hal_pci_init(&pci_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_pci_init should succeed");

    ret = hal_power_init(&power_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_init should succeed");

    /* Initialize HAL context with all devices */
    ret = hal_init_full(&hal_ctx, &nand_dev, &nor_dev, &pci_ctx, &power_ctx);
    TEST_ASSERT(ret == HFSSS_OK, "hal_init_full should succeed");

    /* Test NAND through HAL */
    u8 write_data[4096];
    u8 write_spare[64];
    memset(write_data, 0xBB, sizeof(write_data));
    memset(write_spare, 0xCC, sizeof(write_spare));

    ret = hal_nand_program_sync(&hal_ctx, 0, 0, 0, 0, 0, 0, write_data, write_spare);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_program_sync should succeed");

    u8 read_data[4096];
    u8 read_spare[64];
    memset(read_data, 0, sizeof(read_data));
    memset(read_spare, 0, sizeof(read_spare));

    ret = hal_nand_read_sync(&hal_ctx, 0, 0, 0, 0, 0, 0, read_data, read_spare);
    TEST_ASSERT(ret == HFSSS_OK, "hal_nand_read_sync should succeed");
    TEST_ASSERT(memcmp(read_data, write_data, sizeof(read_data)) == 0,
                "read data should match written data");

    /* Test NOR through HAL */
    u8 nor_write[256];
    u8 nor_read[256];
    memset(nor_write, 0x55, sizeof(nor_write));
    ret = hal_nor_write_sync(&hal_ctx, 0, nor_write, sizeof(nor_write));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_write_sync should succeed");

    memset(nor_read, 0, sizeof(nor_read));
    ret = hal_nor_read_sync(&hal_ctx, 0, nor_read, sizeof(nor_read));
    TEST_ASSERT(ret == HFSSS_OK, "hal_nor_read_sync should succeed");
    TEST_ASSERT(memcmp(nor_read, nor_write, sizeof(nor_read)) == 0,
                "NOR read should match write");

    /* Test Power through HAL */
    ret = hal_power_set_state_sync(&hal_ctx, HAL_POWER_PS1);
    TEST_ASSERT(ret == HFSSS_OK, "hal_power_set_state_sync should succeed");
    TEST_ASSERT(hal_power_get_state_sync(&hal_ctx) == HAL_POWER_PS1, "power state should be PS1");

    /* Test stats */
    struct hal_stats stats;
    hal_get_stats(&hal_ctx, &stats);
    TEST_ASSERT(stats.nand_read_count == 1, "NAND read count should be 1");
    TEST_ASSERT(stats.nand_write_count == 1, "NAND write count should be 1");
    TEST_ASSERT(stats.nor_read_count == 1, "NOR read count should be 1");
    TEST_ASSERT(stats.nor_write_count == 1, "NOR write count should be 1");

    /* Cleanup */
    hal_cleanup(&hal_ctx);
    hal_nand_dev_cleanup(&nand_dev);
    hal_nor_dev_cleanup(&nor_dev);
    hal_pci_cleanup(&pci_ctx);
    hal_power_cleanup(&power_ctx);
    media_cleanup(&media_ctx);

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ==================== REQ-063 AER Tests ==================== */

/* Scenario A: AER submitted first, event posted later -> event
 * consumes outstanding CID, caller gets the completion on post. */
static int test_aer_submit_then_event(void)
{
    printf("\n=== AER: submit-then-event (REQ-063) ===\n");
    struct hal_aer_ctx aer;
    TEST_ASSERT(hal_aer_init(&aer) == HFSSS_OK, "aer: init");

    bool immediate = true;  /* start true so we know it gets overwritten */
    struct nvme_aer_completion c;
    int rc = hal_aer_submit_request(&aer, 0xA001, &immediate, &c);
    TEST_ASSERT(rc == HFSSS_OK, "aer: submit returns OK");
    TEST_ASSERT(immediate == false,
                "aer: submit before any event is not immediate");
    TEST_ASSERT(hal_aer_outstanding_count(&aer) == 1,
                "aer: outstanding_count == 1 after submit");
    TEST_ASSERT(hal_aer_pending_count(&aer) == 0,
                "aer: no pending events yet");

    bool delivered = false;
    rc = hal_aer_post_event(&aer, NVME_AER_TYPE_SMART_HEALTH,
                            NVME_AEI_SMART_TEMPERATURE_THRESHOLD, 0x02,
                            &delivered, &c);
    TEST_ASSERT(rc == HFSSS_OK, "aer: post returns OK");
    TEST_ASSERT(delivered == true,
                "aer: post delivers to waiting AER (immediate completion)");
    TEST_ASSERT(c.cid == 0xA001,
                "aer: completion carries the submitted CID");
    /* DW0 encoding: type<<0 | info<<8 | log<<16 */
    u32 expected_dw0 = ((u32)NVME_AER_TYPE_SMART_HEALTH) |
                       ((u32)NVME_AEI_SMART_TEMPERATURE_THRESHOLD << 8) |
                       ((u32)0x02 << 16);
    TEST_ASSERT(c.cqe.cdw0 == expected_dw0,
                "aer: CQE DW0 packs type/info/log per NVMe spec");
    TEST_ASSERT(hal_aer_outstanding_count(&aer) == 0,
                "aer: outstanding drained by matching event");

    hal_aer_cleanup(&aer);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Scenario B: Event posted first, AER submitted later -> pending ring
 * holds event, next submit completes immediately. */
static int test_aer_event_then_submit(void)
{
    printf("\n=== AER: event-then-submit (REQ-063) ===\n");
    struct hal_aer_ctx aer;
    hal_aer_init(&aer);

    bool delivered = true;  /* start true to verify override */
    struct nvme_aer_completion c;
    int rc = hal_aer_post_event(&aer, NVME_AER_TYPE_ERROR,
                                NVME_AEI_SMART_NVM_SUBSYS_RELIABILITY, 0x01,
                                &delivered, &c);
    TEST_ASSERT(rc == HFSSS_OK, "aer: post OK with no outstanding AER");
    TEST_ASSERT(delivered == false,
                "aer: post not delivered -> event buffered in ring");
    TEST_ASSERT(hal_aer_pending_count(&aer) == 1,
                "aer: pending_count == 1 after buffered event");

    bool immediate = false;
    rc = hal_aer_submit_request(&aer, 0xB002, &immediate, &c);
    TEST_ASSERT(rc == HFSSS_OK, "aer: submit OK after buffered event");
    TEST_ASSERT(immediate == true,
                "aer: submit completes immediately when pending non-empty");
    TEST_ASSERT(c.cid == 0xB002,
                "aer: immediate completion carries caller CID");
    TEST_ASSERT((c.cqe.cdw0 & 0x7) == NVME_AER_TYPE_ERROR,
                "aer: CQE type bits match posted event");
    TEST_ASSERT(hal_aer_pending_count(&aer) == 0,
                "aer: pending drained by matching submit");
    TEST_ASSERT(hal_aer_outstanding_count(&aer) == 0,
                "aer: outstanding empty after immediate completion");

    hal_aer_cleanup(&aer);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Submit 16 AERs -> all queued. 17th returns NOSPC. */
static int test_aer_outstanding_overflow(void)
{
    printf("\n=== AER: outstanding overflow (REQ-063) ===\n");
    struct hal_aer_ctx aer;
    hal_aer_init(&aer);

    struct nvme_aer_completion c;
    bool immediate;
    for (u32 i = 0; i < AER_REQUEST_MAX; i++) {
        int rc = hal_aer_submit_request(&aer, (u16)(0x1000 + i),
                                        &immediate, &c);
        TEST_ASSERT(rc == HFSSS_OK && !immediate,
                    "aer: submit #N queued OK while under limit");
    }
    TEST_ASSERT(hal_aer_outstanding_count(&aer) == AER_REQUEST_MAX,
                "aer: outstanding_count reaches AER_REQUEST_MAX");

    int rc = hal_aer_submit_request(&aer, 0xFFFE, &immediate, &c);
    TEST_ASSERT(rc == HFSSS_ERR_NOSPC,
                "aer: 17th submit returns NOSPC");

    hal_aer_cleanup(&aer);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Controller reset drains outstanding AERs with SC=COMMAND ABORTED. */
static int test_aer_abort_on_reset(void)
{
    printf("\n=== AER: abort_pending on reset (REQ-063) ===\n");
    struct hal_aer_ctx aer;
    hal_aer_init(&aer);

    bool immediate;
    struct nvme_aer_completion ignore;
    hal_aer_submit_request(&aer, 0x2001, &immediate, &ignore);
    hal_aer_submit_request(&aer, 0x2002, &immediate, &ignore);
    hal_aer_submit_request(&aer, 0x2003, &immediate, &ignore);
    TEST_ASSERT(hal_aer_outstanding_count(&aer) == 3,
                "aer: 3 AERs outstanding before abort");

    struct nvme_aer_completion aborts[4];
    u32 n = hal_aer_abort_pending(&aer, aborts, 4);
    TEST_ASSERT(n == 3, "aer: abort returns count of aborted AERs");
    TEST_ASSERT(hal_aer_outstanding_count(&aer) == 0,
                "aer: outstanding empty after abort");
    /* Completions should carry SC=CMD_ABORT_REQUESTED. */
    u16 expected = NVME_BUILD_STATUS(NVME_SC_CMD_ABORT_REQUESTED,
                                     NVME_STATUS_TYPE_GENERIC);
    TEST_ASSERT(aborts[0].cqe.status == expected,
                "aer: abort CQE carries SC=0x07 (CMD_ABORT_REQUESTED)");
    TEST_ASSERT(aborts[0].cid == 0x2001,
                "aer: abort preserves FIFO order (first-in first aborted)");
    TEST_ASSERT(aborts[1].cid == 0x2002,
                "aer: abort second CID matches second submit");
    TEST_ASSERT(aborts[2].cid == 0x2003,
                "aer: abort third CID matches third submit");

    hal_aer_cleanup(&aer);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* DW0 encoding golden vector. */
static int test_aer_dw0_encoding(void)
{
    printf("\n=== AER: DW0 encoding (REQ-063) ===\n");
    u32 dw0 = hal_aer_dw0_encode(NVME_AER_TYPE_SMART_HEALTH,
                                 NVME_AEI_SMART_SPARE_BELOW_THRESHOLD,
                                 0x02 /* SMART log */);
    TEST_ASSERT((dw0 & 0x7) == NVME_AER_TYPE_SMART_HEALTH,
                "aer: DW0 bits[2:0] = type");
    TEST_ASSERT(((dw0 >> 8) & 0xFF) == NVME_AEI_SMART_SPARE_BELOW_THRESHOLD,
                "aer: DW0 bits[15:8] = info");
    TEST_ASSERT(((dw0 >> 16) & 0xFF) == 0x02,
                "aer: DW0 bits[23:16] = log page id");
    TEST_ASSERT((dw0 >> 24) == 0,
                "aer: DW0 bits[31:24] remain zero (reserved)");
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ==================== REQ-064 PCIe Link State Tests ==================== */

/* HA-008 / HA-009: L0 -> L0s -> L0 and L0 -> L1 -> L0 are legal. */
static int test_pcie_link_basic_state_machine(void)
{
    printf("\n=== PCIe link: basic L0/L0s/L1 transitions (REQ-064) ===\n");
    struct pcie_link_ctx link;
    TEST_ASSERT(pcie_link_init(&link) == HFSSS_OK, "pcie: init");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: starts in L0");

    TEST_ASSERT(pcie_link_enter_l0s(&link) == HFSSS_OK, "pcie: L0->L0s");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0S,
                "pcie: now in L0s");
    TEST_ASSERT(pcie_link_exit_l0s(&link) == HFSSS_OK, "pcie: L0s->L0");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: back in L0 after L0s exit");

    TEST_ASSERT(pcie_link_enter_l1(&link) == HFSSS_OK, "pcie: L0->L1");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L1,
                "pcie: now in L1");
    TEST_ASSERT(pcie_link_transition(&link, PCIE_LINK_L2) == HFSSS_OK,
                "pcie: L1->L2 legal");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L2, "pcie: in L2");
    TEST_ASSERT(pcie_link_transition(&link, PCIE_LINK_L0) == HFSSS_OK,
                "pcie: L2->L0 recovery legal");
    TEST_ASSERT(pcie_link_transition_count(&link) == 5,
                "pcie: transition_count matches edges taken");

    pcie_link_cleanup(&link);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HA-010: L0s -> L2 is illegal; L1 -> L0s is illegal. */
static int test_pcie_link_illegal_transitions_rejected(void)
{
    printf("\n=== PCIe link: illegal edges rejected (REQ-064) ===\n");
    struct pcie_link_ctx link;
    pcie_link_init(&link);

    /* L0 -> L0s, then attempt L0s -> L2 (illegal). */
    pcie_link_enter_l0s(&link);
    int rc = pcie_link_transition(&link, PCIE_LINK_L2);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "pcie: L0s->L2 returns INVAL");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0S,
                "pcie: state unchanged after rejected edge");

    /* Return to L0, then L0->L1, then try L1->L0s (illegal). */
    pcie_link_exit_l0s(&link);
    pcie_link_enter_l1(&link);
    rc = pcie_link_transition(&link, PCIE_LINK_L0S);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL, "pcie: L1->L0s returns INVAL");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L1,
                "pcie: state still L1 after rejected edge");
    TEST_ASSERT(pcie_link_rejected_count(&link) == 2,
                "pcie: rejected_transitions counter matches attempts");

    pcie_link_cleanup(&link);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HA-011: hot reset legal from any L* state, lands in L0. */
static int test_pcie_link_hot_reset_from_any_state(void)
{
    printf("\n=== PCIe link: hot reset from L0/L0s/L1/L2 (REQ-064) ===\n");
    struct pcie_link_ctx link;
    pcie_link_init(&link);

    /* From L0 */
    TEST_ASSERT(pcie_hot_reset(&link) == HFSSS_OK,
                "pcie: hot_reset from L0 OK");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: state L0 after hot_reset from L0");

    /* From L0s */
    pcie_link_enter_l0s(&link);
    TEST_ASSERT(pcie_hot_reset(&link) == HFSSS_OK,
                "pcie: hot_reset from L0s OK");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: state L0 after hot_reset from L0s");

    /* From L1 */
    pcie_link_enter_l1(&link);
    TEST_ASSERT(pcie_hot_reset(&link) == HFSSS_OK,
                "pcie: hot_reset from L1 OK");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: state L0 after hot_reset from L1");

    /* From L2 */
    pcie_link_enter_l1(&link);
    pcie_link_transition(&link, PCIE_LINK_L2);
    TEST_ASSERT(pcie_hot_reset(&link) == HFSSS_OK,
                "pcie: hot_reset from L2 OK");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: state L0 after hot_reset from L2");

    pcie_link_cleanup(&link);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* HA-012: FLR only legal from L0. Attempting from L1 is rejected. */
static int test_pcie_link_flr_only_from_l0(void)
{
    printf("\n=== PCIe link: FLR only from L0 (REQ-064) ===\n");
    struct pcie_link_ctx link;
    pcie_link_init(&link);

    TEST_ASSERT(pcie_flr(&link) == HFSSS_OK,
                "pcie: FLR from L0 OK");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "pcie: state L0 after FLR (lands back in L0)");
    TEST_ASSERT(link.flr_in_progress == false,
                "pcie: flr_in_progress cleared after FLR returns");

    /* FLR from L1 must be rejected — host must raise link first. */
    pcie_link_enter_l1(&link);
    int rc = pcie_flr(&link);
    TEST_ASSERT(rc == HFSSS_ERR_INVAL,
                "pcie: FLR from L1 rejected with INVAL");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L1,
                "pcie: state still L1 after rejected FLR");

    pcie_link_cleanup(&link);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* ASPM policy setter accepts the four spec values, rejects out-of-range. */
static int test_pcie_link_aspm_policy(void)
{
    printf("\n=== PCIe link: ASPM policy setter (REQ-064) ===\n");
    struct pcie_link_ctx link;
    pcie_link_init(&link);

    TEST_ASSERT(link.aspm_policy == PCIE_ASPM_L0S_L1,
                "pcie: default ASPM is L0s+L1");
    TEST_ASSERT(pcie_link_set_aspm_policy(&link, PCIE_ASPM_DISABLED) == HFSSS_OK,
                "pcie: set ASPM DISABLED OK");
    TEST_ASSERT(link.aspm_policy == PCIE_ASPM_DISABLED,
                "pcie: aspm_policy reflects setter");
    TEST_ASSERT(pcie_link_set_aspm_policy(&link, PCIE_ASPM_L0S) == HFSSS_OK,
                "pcie: set ASPM L0s OK");
    TEST_ASSERT(pcie_link_set_aspm_policy(&link, PCIE_ASPM_L1) == HFSSS_OK,
                "pcie: set ASPM L1 OK");
    TEST_ASSERT(pcie_link_set_aspm_policy(&link, 99) == HFSSS_ERR_INVAL,
                "pcie: out-of-range ASPM rejected");

    pcie_link_cleanup(&link);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Fix-4: ASPM policy must actually gate L0s / L1 entry. The earlier
 * implementation stored the enum but never consulted it — enter_l0s
 * worked even under PCIE_ASPM_DISABLED. */
static int test_pcie_link_aspm_policy_gates_entry(void)
{
    printf("\n=== PCIe link: ASPM policy gates L0s/L1 entry (Fix-4) ===\n");
    struct pcie_link_ctx link;
    pcie_link_init(&link);

    /* DISABLED blocks both. */
    pcie_link_set_aspm_policy(&link, PCIE_ASPM_DISABLED);
    int rc = pcie_link_enter_l0s(&link);
    TEST_ASSERT(rc == HFSSS_ERR_AUTH,
                "aspm: L0s refused under DISABLED");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0,
                "aspm: state still L0 after refused L0s");
    rc = pcie_link_enter_l1(&link);
    TEST_ASSERT(rc == HFSSS_ERR_AUTH,
                "aspm: L1 refused under DISABLED");
    TEST_ASSERT(pcie_link_rejected_count(&link) == 2,
                "aspm: both refusals counted");

    /* ASPM_L0S permits L0s but blocks L1. */
    pcie_link_set_aspm_policy(&link, PCIE_ASPM_L0S);
    rc = pcie_link_enter_l0s(&link);
    TEST_ASSERT(rc == HFSSS_OK,
                "aspm: L0s OK under ASPM_L0S policy");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L0S,
                "aspm: actually entered L0s");
    pcie_link_exit_l0s(&link);

    rc = pcie_link_enter_l1(&link);
    TEST_ASSERT(rc == HFSSS_ERR_AUTH,
                "aspm: L1 refused under ASPM_L0S policy");

    /* ASPM_L1 permits L1 but blocks L0s. */
    pcie_link_set_aspm_policy(&link, PCIE_ASPM_L1);
    rc = pcie_link_enter_l0s(&link);
    TEST_ASSERT(rc == HFSSS_ERR_AUTH,
                "aspm: L0s refused under ASPM_L1 policy");
    rc = pcie_link_enter_l1(&link);
    TEST_ASSERT(rc == HFSSS_OK,
                "aspm: L1 OK under ASPM_L1 policy");
    TEST_ASSERT(pcie_link_get_state(&link) == PCIE_LINK_L1,
                "aspm: actually entered L1");
    pcie_link_exit_l1(&link);

    /* ASPM_L0S_L1 permits both (default). */
    pcie_link_set_aspm_policy(&link, PCIE_ASPM_L0S_L1);
    rc = pcie_link_enter_l0s(&link);
    TEST_ASSERT(rc == HFSSS_OK,
                "aspm: L0s OK under ASPM_L0S_L1 (default)");
    pcie_link_exit_l0s(&link);
    rc = pcie_link_enter_l1(&link);
    TEST_ASSERT(rc == HFSSS_OK,
                "aspm: L1 OK under ASPM_L0S_L1");

    pcie_link_cleanup(&link);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

/* Main */
int main(void)
{
    printf("========================================\n");
    printf("HFSSS HAL Module Tests\n");
    printf("========================================\n");

    int result = 0;

    (void)result; /* Suppress unused variable warning */

    test_hal_nand_dev();
    test_hal_nor();
    test_hal_power();
    test_hal_pci();
    test_hal();
    test_aer_submit_then_event();
    test_aer_event_then_submit();
    test_aer_outstanding_overflow();
    test_aer_abort_on_reset();
    test_aer_dw0_encoding();
    test_pcie_link_basic_state_machine();
    test_pcie_link_illegal_transitions_rejected();
    test_pcie_link_hot_reset_from_any_state();
    test_pcie_link_flr_only_from_l0();
    test_pcie_link_aspm_policy();
    test_pcie_link_aspm_policy_gates_entry();

    printf("\n========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    } else {
        printf("\n  [FAILURE] Some tests failed!\n");
        return 1;
    }
}
