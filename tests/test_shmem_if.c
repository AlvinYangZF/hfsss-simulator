#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "controller/shmem_if.h"

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
} while (0)

struct shmem_fixture {
    char name[128];
    struct shmem_layout *backing;
    int backing_fd;
    struct shmem_layout *sut;
    int sut_fd;
};

static void fixture_cleanup(struct shmem_fixture *fx)
{
    if (!fx) {
        return;
    }

    if (fx->sut) {
        shmem_if_close(fx->sut, fx->sut_fd);
        fx->sut = NULL;
        fx->sut_fd = -1;
    }

    if (fx->backing && fx->backing != MAP_FAILED) {
        munmap(fx->backing, sizeof(*fx->backing));
        fx->backing = NULL;
    }

    if (fx->backing_fd >= 0) {
        close(fx->backing_fd);
        fx->backing_fd = -1;
    }

    if (fx->name[0] != '\0') {
        shm_unlink(fx->name);
        fx->name[0] = '\0';
    }
}

static int fixture_init(struct shmem_fixture *fx)
{
    int ret;

    if (!fx) {
        return HFSSS_ERR_INVAL;
    }

    memset(fx, 0, sizeof(*fx));
    fx->backing_fd = -1;
    fx->sut_fd = -1;

    snprintf(fx->name, sizeof(fx->name), "/hf_%x_%llx",
             (unsigned int)getpid(), (unsigned long long)get_time_ns());

    fx->backing_fd = shm_open(fx->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fx->backing_fd < 0) {
        fx->name[0] = '\0';
        return HFSSS_ERR_IO;
    }

    if (ftruncate(fx->backing_fd, sizeof(struct shmem_layout)) != 0) {
        fixture_cleanup(fx);
        return HFSSS_ERR_IO;
    }

    fx->backing = mmap(NULL, sizeof(struct shmem_layout),
                       PROT_READ | PROT_WRITE, MAP_SHARED, fx->backing_fd, 0);
    if (fx->backing == MAP_FAILED) {
        fx->backing = NULL;
        fixture_cleanup(fx);
        return HFSSS_ERR_NOMEM;
    }

    fx->backing->header.slot_count = RING_BUFFER_SLOTS;
    fx->backing->header.slot_size = sizeof(struct ring_slot);

    ret = shmem_if_open(fx->name, &fx->sut, &fx->sut_fd);
    if (ret != HFSSS_OK) {
        fixture_cleanup(fx);
        return ret;
    }

    return HFSSS_OK;
}

static int test_open_invalid_args(void)
{
    printf("\n=== shmem_if_open Argument Validation ===\n");

    struct shmem_layout *shmem = NULL;
    int fd = -1;

    TEST_ASSERT(shmem_if_open(NULL, &shmem, &fd) == HFSSS_ERR_INVAL,
                "shmem_if_open should reject NULL path");
    TEST_ASSERT(shmem_if_open("/hfsss_missing", NULL, &fd) == HFSSS_ERR_INVAL,
                "shmem_if_open should reject NULL shmem_out");
    TEST_ASSERT(shmem_if_open("/hfsss_missing", &shmem, NULL) == HFSSS_ERR_INVAL,
                "shmem_if_open should reject NULL fd_out");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_open_missing_object(void)
{
    printf("\n=== shmem_if_open Missing Object ===\n");

    struct shmem_layout *shmem = NULL;
    int fd = -1;
    char name[128];

    snprintf(name, sizeof(name), "/hm_%x_%llx",
             (unsigned int)getpid(), (unsigned long long)get_time_ns());

    TEST_ASSERT(shmem_if_open(name, &shmem, &fd) == HFSSS_ERR_NOTSUPP,
                "shmem_if_open should report missing shared memory");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_open_close_real_shmem(void)
{
    printf("\n=== shmem_if_open/shmem_if_close Real Shared Memory ===\n");

    struct shmem_fixture fx;
    int ret = fixture_init(&fx);

    TEST_ASSERT(ret == HFSSS_OK, "fixture_init should create and map shared memory");
    if (ret == HFSSS_OK) {
        TEST_ASSERT(fx.sut != NULL, "shmem_if_open should return a mapped layout");
        TEST_ASSERT(fx.sut_fd >= 0, "shmem_if_open should return a valid fd");
        TEST_ASSERT(fx.sut->header.slot_count == RING_BUFFER_SLOTS,
                    "mapped header should be shared with creator");
        TEST_ASSERT(fx.sut->header.slot_size == sizeof(struct ring_slot),
                    "mapped slot size should match ring slot layout");
    }

    fixture_cleanup(&fx);

    shmem_if_close(NULL, -1);
    TEST_ASSERT(1, "shmem_if_close should tolerate invalid arguments");

    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_receive_empty_and_not_ready(void)
{
    printf("\n=== shmem_if_receive_cmd Empty/Not-Ready ===\n");

    struct shmem_fixture fx;
    struct nvme_cmd_from_kern cmd;
    int ret = fixture_init(&fx);

    TEST_ASSERT(ret == HFSSS_OK, "fixture_init should succeed");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    memset(&cmd, 0, sizeof(cmd));
    TEST_ASSERT(shmem_if_receive_cmd(fx.sut, &cmd) == HFSSS_ERR_AGAIN,
                "receive on empty ring should return AGAIN");
    TEST_ASSERT(fx.backing->header.cons_idx == 0 && fx.backing->header.cons_seq == 0,
                "empty receive should not advance consumer state");

    fx.backing->header.prod_idx = 1;
    fx.backing->slots[0].ready = 0;
    TEST_ASSERT(shmem_if_receive_cmd(fx.sut, &cmd) == HFSSS_ERR_AGAIN,
                "receive on not-ready slot should return AGAIN");
    TEST_ASSERT(fx.backing->header.cons_idx == 0 && fx.backing->header.cons_seq == 0,
                "not-ready receive should not advance consumer state");

    TEST_ASSERT(shmem_if_receive_cmd(NULL, &cmd) == HFSSS_ERR_INVAL,
                "receive should reject NULL shmem");
    TEST_ASSERT(shmem_if_receive_cmd(fx.sut, NULL) == HFSSS_ERR_INVAL,
                "receive should reject NULL cmd");

    fixture_cleanup(&fx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_receive_success_and_wrap(void)
{
    printf("\n=== shmem_if_receive_cmd Success/Wrap ===\n");

    struct shmem_fixture fx;
    struct nvme_cmd_from_kern cmd;
    struct ring_slot *slot;
    int ret = fixture_init(&fx);

    TEST_ASSERT(ret == HFSSS_OK, "fixture_init should succeed");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    fx.backing->header.prod_idx = 1;
    fx.backing->header.cons_idx = 0;
    slot = &fx.backing->slots[0];
    slot->cmd.cmd_type = CMD_NVME_IO;
    slot->cmd.cmd_id = 42;
    slot->cmd.sqid = 7;
    slot->cmd.cdw0_15[0] = 0xdeadbeef;
    slot->ready = 1;

    memset(&cmd, 0, sizeof(cmd));
    TEST_ASSERT(shmem_if_receive_cmd(fx.sut, &cmd) == HFSSS_OK,
                "receive should consume a ready slot");
    TEST_ASSERT(cmd.cmd_type == CMD_NVME_IO && cmd.cmd_id == 42 && cmd.sqid == 7,
                "receive should copy command payload");
    TEST_ASSERT(cmd.cdw0_15[0] == 0xdeadbeef,
                "receive should preserve command words");
    TEST_ASSERT(slot->ready == 0, "receive should clear slot ready flag");
    TEST_ASSERT(fx.backing->header.cons_idx == 1 && fx.backing->header.cons_seq == 1,
                "receive should advance consumer index and sequence");

    fx.backing->header.cons_idx = RING_BUFFER_SLOTS - 1;
    fx.backing->header.prod_idx = RING_BUFFER_SLOTS;
    fx.backing->header.cons_seq = 0;
    slot = &fx.backing->slots[RING_BUFFER_SLOTS - 1];
    memset(&slot->cmd, 0, sizeof(slot->cmd));
    slot->cmd.cmd_id = 99;
    slot->ready = 1;

    memset(&cmd, 0, sizeof(cmd));
    TEST_ASSERT(shmem_if_receive_cmd(fx.sut, &cmd) == HFSSS_OK,
                "receive should handle consumer index wrap");
    TEST_ASSERT(cmd.cmd_id == 99, "wrapped receive should copy the wrapped slot command");
    TEST_ASSERT(fx.backing->header.cons_idx == 0 && fx.backing->header.cons_seq == 1,
                "wrapped receive should wrap consumer index to zero");

    fixture_cleanup(&fx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

static int test_send_cpl_busy_success_and_wrap(void)
{
    printf("\n=== shmem_if_send_cpl Busy/Success/Wrap ===\n");

    struct shmem_fixture fx;
    struct nvme_cpl_to_kern cpl;
    struct ring_slot *slot;
    int ret = fixture_init(&fx);

    TEST_ASSERT(ret == HFSSS_OK, "fixture_init should succeed");
    if (ret != HFSSS_OK) {
        return TEST_FAIL;
    }

    memset(&cpl, 0, sizeof(cpl));
    cpl.cmd_id = 7;

    slot = &fx.backing->slots[0];
    fx.backing->header.prod_idx = 0;
    slot->done = 1;
    TEST_ASSERT(shmem_if_send_cpl(fx.sut, &cpl) == HFSSS_ERR_AGAIN,
                "send_cpl should return AGAIN when slot is already done");
    TEST_ASSERT(fx.backing->header.prod_idx == 0 && fx.backing->header.prod_seq == 0,
                "busy completion should not advance producer state");

    slot->done = 0;
    slot->ready = 1;
    TEST_ASSERT(shmem_if_send_cpl(fx.sut, &cpl) == HFSSS_OK,
                "send_cpl should mark an available slot done");
    TEST_ASSERT(slot->ready == 0 && slot->done == 1,
                "send_cpl should clear ready and mark done");
    TEST_ASSERT(fx.backing->header.prod_idx == 1 && fx.backing->header.prod_seq == 1,
                "send_cpl should advance producer index and sequence");

    fx.backing->header.prod_idx = RING_BUFFER_SLOTS - 1;
    fx.backing->header.prod_seq = 0;
    slot = &fx.backing->slots[RING_BUFFER_SLOTS - 1];
    slot->ready = 1;
    slot->done = 0;
    TEST_ASSERT(shmem_if_send_cpl(fx.sut, &cpl) == HFSSS_OK,
                "send_cpl should handle producer index wrap");
    TEST_ASSERT(fx.backing->header.prod_idx == 0 && fx.backing->header.prod_seq == 1,
                "wrapped send_cpl should wrap producer index to zero");

    TEST_ASSERT(shmem_if_send_cpl(NULL, &cpl) == HFSSS_ERR_INVAL,
                "send_cpl should reject NULL shmem");
    TEST_ASSERT(shmem_if_send_cpl(fx.sut, NULL) == HFSSS_ERR_INVAL,
                "send_cpl should reject NULL completion");

    fixture_cleanup(&fx);
    return tests_failed > 0 ? TEST_FAIL : TEST_PASS;
}

int main(void)
{
    printf("========================================\n");
    printf("HFSSS Shared Memory Interface Tests\n");
    printf("========================================\n");

    test_open_invalid_args();
    test_open_missing_object();
    test_open_close_real_shmem();
    test_receive_empty_and_not_ready();
    test_receive_success_and_wrap();
    test_send_cpl_busy_success_and_wrap();

    printf("========================================\n");
    printf("Test Summary\n");
    printf("========================================\n");
    printf("  Total:  %d\n", tests_run);
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("========================================\n");

    if (tests_failed == 0) {
        printf("\n  [SUCCESS] All tests passed!\n");
        return 0;
    }

    printf("\n  [FAILURE] Some tests failed!\n");
    return 1;
}
