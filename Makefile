# HFSSS - High Fidelity Full-Stack SSD Simulator Makefile

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -std=gnu11 -fPIC
CFLAGS += -Iinclude -Iinclude/common -Iinclude/media -Iinclude/hal -Iinclude/ftl -Iinclude/controller -Iinclude/pcie -Isrc/vhost
CFLAGS += -MMD -MP
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lpthread -lrt
else
    LDFLAGS = -lpthread
endif

# Coverage build variant (GCC --coverage instrumentation)
COVERAGE ?= 0
ifeq ($(COVERAGE),1)
    CFLAGS := $(filter-out -O2, $(CFLAGS)) --coverage -O0
    LDFLAGS += --coverage
    BUILD_DIR = build-cov
else
    BUILD_DIR = build
endif

# AddressSanitizer build variant
ASAN ?= 0
ifeq ($(ASAN),1)
    CFLAGS += -fsanitize=address -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=address
    BUILD_DIR := $(BUILD_DIR)-asan
endif

# UndefinedBehaviorSanitizer build variant
UBSAN ?= 0
ifeq ($(UBSAN),1)
    CFLAGS += -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
    LDFLAGS += -fsanitize=undefined
    BUILD_DIR := $(BUILD_DIR)-ubsan
endif

# ThreadSanitizer build variant
TSAN ?= 0
ifeq ($(TSAN),1)
    CFLAGS += -fsanitize=thread -fno-omit-frame-pointer -O1
    LDFLAGS += -fsanitize=thread
    BUILD_DIR := $(BUILD_DIR)-tsan
endif

# Directories
SRC_DIR = src
COMMON_SRC = $(SRC_DIR)/common
MEDIA_SRC = $(SRC_DIR)/media
HAL_SRC = $(SRC_DIR)/hal
FTL_SRC = $(SRC_DIR)/ftl
CTRL_SRC = $(SRC_DIR)/controller
PCIE_SRC = $(SRC_DIR)/pcie
SSSIM_SRC = $(SRC_DIR)
TEST_DIR = tests
LIB_DIR = $(BUILD_DIR)/lib
BIN_DIR = $(BUILD_DIR)/bin

# Source files
COMMON_SRCS = $(wildcard $(COMMON_SRC)/*.c)
MEDIA_SRCS = $(wildcard $(MEDIA_SRC)/*.c)
HAL_SRCS = $(wildcard $(HAL_SRC)/*.c)
FTL_SRCS = $(wildcard $(FTL_SRC)/*.c)
CTRL_SRCS = $(wildcard $(CTRL_SRC)/*.c)
PCIE_SRCS = $(wildcard $(PCIE_SRC)/*.c)
SSSIM_SRCS = $(SSSIM_SRC)/sssim.c

# Object files
COMMON_OBJS = $(COMMON_SRCS:$(SRC_DIR)/common/%.c=$(BUILD_DIR)/common/%.o)
MEDIA_OBJS = $(MEDIA_SRCS:$(SRC_DIR)/media/%.c=$(BUILD_DIR)/media/%.o)
HAL_OBJS = $(HAL_SRCS:$(SRC_DIR)/hal/%.c=$(BUILD_DIR)/hal/%.o)
FTL_OBJS = $(FTL_SRCS:$(SRC_DIR)/ftl/%.c=$(BUILD_DIR)/ftl/%.o)
CTRL_OBJS = $(CTRL_SRCS:$(SRC_DIR)/controller/%.c=$(BUILD_DIR)/controller/%.o)
PCIE_OBJS = $(PCIE_SRCS:$(SRC_DIR)/pcie/%.c=$(BUILD_DIR)/pcie/%.o)
SSSIM_OBJS = $(SSSIM_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Auto-generated header dependency files
DEP_FILES = $(COMMON_OBJS:.o=.d) $(MEDIA_OBJS:.o=.d) $(HAL_OBJS:.o=.d) \
            $(FTL_OBJS:.o=.d) $(CTRL_OBJS:.o=.d) $(PCIE_OBJS:.o=.d) \
            $(SSSIM_OBJS:.o=.d)
-include $(DEP_FILES)

# Libraries
LIBHFSSS_COMMON = $(LIB_DIR)/libhfsss-common.a
LIBHFSSS_MEDIA = $(LIB_DIR)/libhfsss-media.a
LIBHFSSS_HAL = $(LIB_DIR)/libhfsss-hal.a
LIBHFSSS_FTL = $(LIB_DIR)/libhfsss-ftl.a
LIBHFSSS_CTRL = $(LIB_DIR)/libhfsss-controller.a
LIBHFSSS_PCIE = $(LIB_DIR)/libhfsss-pcie.a
LIBHFSSS_SSSIM = $(LIB_DIR)/libhfsss-sssim.a

# Test programs
TEST_COMMON = $(BIN_DIR)/test_common
TEST_MEDIA = $(BIN_DIR)/test_media
TEST_HAL = $(BIN_DIR)/test_hal
TEST_FTL = $(BIN_DIR)/test_ftl
TEST_CTRL = $(BIN_DIR)/test_controller
TEST_SHMEM_IF = $(BIN_DIR)/test_shmem_if
TEST_PCIE = $(BIN_DIR)/test_pcie_nvme
TEST_SSSIM = $(BIN_DIR)/test_sssim
TEST_NVME_USPACE = $(BIN_DIR)/test_nvme_uspace
TEST_BOOT = $(BIN_DIR)/test_boot
TEST_NOR = $(BIN_DIR)/test_nor_flash
TEST_FTL_REL = $(BIN_DIR)/test_ftl_reliability
TEST_RT = $(BIN_DIR)/test_rt_services
TEST_OOB = $(BIN_DIR)/test_oob
TEST_CONFIG = $(BIN_DIR)/test_config
TEST_FAULT = $(BIN_DIR)/test_fault_inject
TEST_RELIABILITY = $(BIN_DIR)/test_reliability
HFSSS_CTRL = $(BIN_DIR)/hfsss-ctrl
TEST_DSM = $(BIN_DIR)/test_dsm
TEST_PRP = $(BIN_DIR)/test_prp
STRESS_RW = $(BIN_DIR)/stress_rw
STRESS_MIXED = $(BIN_DIR)/stress_mixed
STRESS_MIXED_TRIM = $(BIN_DIR)/stress_mixed_trim
TEST_FTL_INT = $(BIN_DIR)/test_ftl_integrity
STRESS_ADMIN_MIX = $(BIN_DIR)/stress_admin_mix
TEST_SB = $(BIN_DIR)/test_superblock
TEST_POWER_CYCLE = $(BIN_DIR)/test_power_cycle
TEST_FOUNDATION = $(BIN_DIR)/test_foundation
TEST_T10PI = $(BIN_DIR)/test_t10_pi
SYSTEST_DI = $(BIN_DIR)/systest_data_integrity
SYSTEST_NC = $(BIN_DIR)/systest_nvme_compliance
SYSTEST_EB = $(BIN_DIR)/systest_error_boundary
TEST_UPLP = $(BIN_DIR)/test_uplp
TEST_QOS = $(BIN_DIR)/test_qos
TEST_SECURITY = $(BIN_DIR)/test_security
TEST_MULTI_NS = $(BIN_DIR)/test_multi_ns
TEST_THERMAL_TEL = $(BIN_DIR)/test_thermal_telemetry
STRESS_ENTERPRISE = $(BIN_DIR)/stress_enterprise
TEST_PROC = $(BIN_DIR)/test_proc_interface
STRESS_STABILITY = $(BIN_DIR)/stress_stability
TEST_LARGE_CAP = $(BIN_DIR)/test_large_capacity
TEST_IO_QUEUE = $(BIN_DIR)/test_io_queue
TEST_TAA = $(BIN_DIR)/test_taa
TEST_MT_FTL = $(BIN_DIR)/test_mt_ftl
TEST_GC_MT = $(BIN_DIR)/test_gc_mt
TEST_INFLIGHT = $(BIN_DIR)/test_inflight_pool
TEST_MSGQ = $(BIN_DIR)/test_msgqueue
TEST_NVME_ADMIN = $(BIN_DIR)/test_nvme_admin_cmds
TEST_NVME_IO = $(BIN_DIR)/test_nvme_io_cmds

# Perf validation library and test
TEST_PERF = $(BIN_DIR)/test_perf_validation
LIBHFSSS_PERF = $(LIB_DIR)/libhfsss-perf.a
PERF_SRC = $(SRC_DIR)/perf
PERF_SRCS = $(wildcard $(PERF_SRC)/*.c)
PERF_OBJS = $(PERF_SRCS:$(SRC_DIR)/perf/%.c=$(BUILD_DIR)/perf/%.o)

# vhost-user-blk server (WIP — source not yet committed; build rule disabled)
VHOST_SRC = $(SRC_DIR)/vhost
# VHOST_SRCS = $(VHOST_SRC)/vhost_user_blk.c
# VHOST_OBJS = $(VHOST_SRCS:$(SRC_DIR)/vhost/%.c=$(BUILD_DIR)/vhost/%.o)
# HFSSS_VHOST = $(BIN_DIR)/hfsss-vhost-blk
HFSSS_IMG_EXPORT = $(BIN_DIR)/hfsss-img-export
HFSSS_NBD = $(BIN_DIR)/hfsss-nbd-server
TEST_VHOST = $(BIN_DIR)/test_vhost_proto

# Coverage build set:
# - UT coverage executes every built test_/systest_/stress_ binary
# - E2E coverage requires hfsss-nbd-server
# Do not pull in vhost targets here: the vhost-user-blk path is intentionally
# outside the current UT/E2E coverage scope and is not buildable on mainline.
COVERAGE_BIN_DIR = build-cov/bin
COVERAGE_UT_BINS = $(COVERAGE_BIN_DIR)/test_common $(COVERAGE_BIN_DIR)/test_media \
	$(COVERAGE_BIN_DIR)/test_hal $(COVERAGE_BIN_DIR)/test_ftl \
	$(COVERAGE_BIN_DIR)/test_controller $(COVERAGE_BIN_DIR)/test_shmem_if \
	$(COVERAGE_BIN_DIR)/test_pcie_nvme \
	$(COVERAGE_BIN_DIR)/test_sssim $(COVERAGE_BIN_DIR)/test_nvme_uspace \
	$(COVERAGE_BIN_DIR)/test_boot $(COVERAGE_BIN_DIR)/test_nor_flash \
	$(COVERAGE_BIN_DIR)/test_ftl_reliability $(COVERAGE_BIN_DIR)/test_rt_services \
	$(COVERAGE_BIN_DIR)/test_oob $(COVERAGE_BIN_DIR)/test_config \
	$(COVERAGE_BIN_DIR)/test_fault_inject $(COVERAGE_BIN_DIR)/test_reliability \
	$(COVERAGE_BIN_DIR)/test_perf_validation $(COVERAGE_BIN_DIR)/test_dsm \
	$(COVERAGE_BIN_DIR)/test_prp $(COVERAGE_BIN_DIR)/stress_rw \
	$(COVERAGE_BIN_DIR)/stress_mixed $(COVERAGE_BIN_DIR)/stress_mixed_trim \
	$(COVERAGE_BIN_DIR)/test_ftl_integrity $(COVERAGE_BIN_DIR)/stress_admin_mix \
	$(COVERAGE_BIN_DIR)/test_superblock $(COVERAGE_BIN_DIR)/test_power_cycle \
	$(COVERAGE_BIN_DIR)/test_foundation $(COVERAGE_BIN_DIR)/test_t10_pi \
	$(COVERAGE_BIN_DIR)/systest_data_integrity $(COVERAGE_BIN_DIR)/systest_nvme_compliance \
	$(COVERAGE_BIN_DIR)/systest_error_boundary $(COVERAGE_BIN_DIR)/test_uplp \
	$(COVERAGE_BIN_DIR)/test_qos $(COVERAGE_BIN_DIR)/test_security \
	$(COVERAGE_BIN_DIR)/test_multi_ns $(COVERAGE_BIN_DIR)/test_thermal_telemetry \
	$(COVERAGE_BIN_DIR)/stress_enterprise $(COVERAGE_BIN_DIR)/test_proc_interface \
	$(COVERAGE_BIN_DIR)/stress_stability $(COVERAGE_BIN_DIR)/test_large_capacity \
	$(COVERAGE_BIN_DIR)/test_io_queue $(COVERAGE_BIN_DIR)/test_taa \
	$(COVERAGE_BIN_DIR)/test_mt_ftl $(COVERAGE_BIN_DIR)/test_gc_mt \
	$(COVERAGE_BIN_DIR)/test_inflight_pool \
	$(COVERAGE_BIN_DIR)/test_msgqueue \
	$(COVERAGE_BIN_DIR)/test_nvme_admin_cmds \
	$(COVERAGE_BIN_DIR)/test_nvme_io_cmds
COVERAGE_E2E_BINS = $(COVERAGE_BIN_DIR)/hfsss-nbd-server
COVERAGE_BINS = $(COVERAGE_UT_BINS) $(COVERAGE_E2E_BINS)

# Targets
.PHONY: all clean directories test systest stress-long help \
	coverage-build coverage-clean coverage-ut coverage-e2e coverage-merge coverage coverage-selftest \
	qemu-blackbox qemu-blackbox-list qemu-blackbox-ci qemu-blackbox-soak pre-checkin

all: directories $(LIBHFSSS_COMMON) $(LIBHFSSS_MEDIA) $(LIBHFSSS_HAL) $(LIBHFSSS_FTL) $(LIBHFSSS_CTRL) $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_PERF) $(TEST_COMMON) $(TEST_MEDIA) $(TEST_HAL) $(TEST_FTL) $(TEST_CTRL) $(TEST_SHMEM_IF) $(TEST_PCIE) $(TEST_SSSIM) $(TEST_NVME_USPACE) $(TEST_BOOT) $(TEST_NOR) $(TEST_FTL_REL) $(TEST_RT) $(TEST_OOB) $(TEST_CONFIG) $(TEST_FAULT) $(TEST_RELIABILITY) $(TEST_PERF) $(TEST_DSM) $(TEST_PRP) $(STRESS_RW) $(STRESS_MIXED) $(STRESS_MIXED_TRIM) $(HFSSS_CTRL) $(TEST_FTL_INT) $(STRESS_ADMIN_MIX) $(TEST_SB) $(TEST_POWER_CYCLE) $(TEST_FOUNDATION) $(TEST_T10PI) $(SYSTEST_DI) $(SYSTEST_NC) $(SYSTEST_EB) $(TEST_UPLP) $(TEST_QOS) $(TEST_SECURITY) $(TEST_MULTI_NS) $(TEST_THERMAL_TEL) $(STRESS_ENTERPRISE) $(TEST_PROC) $(STRESS_STABILITY) $(HFSSS_IMG_EXPORT) $(HFSSS_NBD) $(TEST_LARGE_CAP) $(TEST_IO_QUEUE) $(TEST_TAA) $(TEST_MT_FTL) $(TEST_GC_MT) $(TEST_INFLIGHT) $(TEST_MSGQ) $(TEST_NVME_ADMIN) $(TEST_NVME_IO)
	@echo "========================================"
	@echo "HFSSS build complete!"
	@echo "========================================"

directories:
	@mkdir -p $(BUILD_DIR)/common
	@mkdir -p $(BUILD_DIR)/media
	@mkdir -p $(BUILD_DIR)/hal
	@mkdir -p $(BUILD_DIR)/ftl
	@mkdir -p $(BUILD_DIR)/controller
	@mkdir -p $(BUILD_DIR)/pcie
	@mkdir -p $(BUILD_DIR)/perf
	@mkdir -p $(BUILD_DIR)/vhost
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(LIB_DIR)
	@mkdir -p $(BIN_DIR)

# Common library
$(BUILD_DIR)/common/%.o: $(SRC_DIR)/common/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_COMMON): $(COMMON_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(COMMON_OBJS)

# Media library
$(BUILD_DIR)/media/%.o: $(SRC_DIR)/media/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_MEDIA): $(MEDIA_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(MEDIA_OBJS)

# HAL library
$(BUILD_DIR)/hal/%.o: $(SRC_DIR)/hal/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_HAL): $(HAL_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(HAL_OBJS)

# FTL library
$(BUILD_DIR)/ftl/%.o: $(SRC_DIR)/ftl/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_FTL): $(FTL_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(FTL_OBJS)

# Controller library
$(BUILD_DIR)/controller/%.o: $(SRC_DIR)/controller/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_CTRL): $(CTRL_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(CTRL_OBJS)

# PCIe/NVMe library
$(BUILD_DIR)/pcie/%.o: $(SRC_DIR)/pcie/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_PCIE): $(PCIE_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(PCIE_OBJS)

# SSSIM library
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_SSSIM): $(SSSIM_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(SSSIM_OBJS)

# Perf validation library
$(BUILD_DIR)/perf/%.o: $(SRC_DIR)/perf/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

$(LIBHFSSS_PERF): $(PERF_OBJS)
	@echo "  AR      $@"
	@ar rcs $@ $(PERF_OBJS)

# Test programs
$(TEST_COMMON): $(TEST_DIR)/test_common.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)

$(TEST_MEDIA): $(TEST_DIR)/test_media.c $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_HAL): $(TEST_DIR)/test_hal.c $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_FTL): $(TEST_DIR)/test_ftl.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_SSSIM): $(TEST_DIR)/test_sssim.c $(LIBHFSSS_SSSIM) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-sssim -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_CTRL): $(TEST_DIR)/test_controller.c $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_SHMEM_IF): $(TEST_DIR)/test_shmem_if.c $(LIBHFSSS_CTRL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-controller -lhfsss-common $(LDFLAGS)

$(TEST_MSGQ): $(TEST_DIR)/test_msgqueue.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)

$(TEST_PCIE): $(TEST_DIR)/test_pcie_nvme.c $(LIBHFSSS_PCIE) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_NVME_ADMIN): $(TEST_DIR)/test_nvme_admin_cmds.c $(LIBHFSSS_PCIE) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_NVME_IO): $(TEST_DIR)/test_nvme_io_cmds.c $(LIBHFSSS_PCIE) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_NVME_USPACE): $(TEST_DIR)/test_nvme_uspace.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_BOOT): $(TEST_DIR)/test_boot.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)

$(TEST_NOR): $(TEST_DIR)/test_nor_flash.c $(SRC_DIR)/media/nor_flash.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -DHFSSS_NOR_TEST_MODE $(TEST_DIR)/test_nor_flash.c $(SRC_DIR)/media/nor_flash.c -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)

$(TEST_FTL_REL): $(TEST_DIR)/test_ftl_reliability.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common $(LDFLAGS)

$(TEST_RT): $(TEST_DIR)/test_rt_services.c $(SRC_DIR)/common/rt_services.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -DHFSSS_TRACE_TEST_MODE $(TEST_DIR)/test_rt_services.c $(SRC_DIR)/common/rt_services.c -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)

$(TEST_OOB): $(TEST_DIR)/test_oob.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common -lm $(LDFLAGS)

$(TEST_CONFIG): $(TEST_DIR)/test_config.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common $(LDFLAGS)

$(TEST_FAULT): $(TEST_DIR)/test_fault_inject.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common -lm $(LDFLAGS)

$(TEST_RELIABILITY): $(TEST_DIR)/test_reliability.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common -lm $(LDFLAGS)

$(TEST_PERF): $(TEST_DIR)/test_perf_validation.c $(LIBHFSSS_PERF) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-perf -lhfsss-common -lm $(LDFLAGS)

$(HFSSS_CTRL): $(SRC_DIR)/tools/hfsss_ctrl.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(STRESS_RW): $(TEST_DIR)/stress_rw.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_DSM): $(TEST_DIR)/test_dsm.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_PRP): $(TEST_DIR)/test_prp.c $(LIBHFSSS_PCIE) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-common $(LDFLAGS)

$(STRESS_MIXED_TRIM): $(TEST_DIR)/stress_mixed_trim.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(STRESS_MIXED): $(TEST_DIR)/stress_mixed.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_FTL_INT): $(TEST_DIR)/test_ftl_integrity.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(STRESS_ADMIN_MIX): $(TEST_DIR)/stress_admin_mix.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_SB): $(TEST_DIR)/test_superblock.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_POWER_CYCLE): $(TEST_DIR)/test_power_cycle.c $(LIBHFSSS_SSSIM) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-sssim -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_FOUNDATION): $(TEST_DIR)/test_foundation.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_T10PI): $(TEST_DIR)/test_t10_pi.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common $(LDFLAGS)

$(TEST_IO_QUEUE): $(TEST_DIR)/test_io_queue.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common -lm $(LDFLAGS)

$(TEST_TAA): $(TEST_DIR)/test_taa.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common -lm $(LDFLAGS)

$(TEST_MT_FTL): $(TEST_DIR)/test_mt_ftl.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_GC_MT): $(TEST_DIR)/test_gc_mt.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_INFLIGHT): $(TEST_DIR)/test_inflight_pool.c $(VHOST_SRC)/nbd_async.c $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $(TEST_DIR)/test_inflight_pool.c $(VHOST_SRC)/nbd_async.c -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(SYSTEST_DI): $(TEST_DIR)/systest_data_integrity.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(SYSTEST_NC): $(TEST_DIR)/systest_nvme_compliance.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(SYSTEST_EB): $(TEST_DIR)/systest_error_boundary.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_UPLP): $(TEST_DIR)/test_uplp.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common -lm $(LDFLAGS)

$(TEST_QOS): $(TEST_DIR)/test_qos.c $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-controller -lhfsss-ftl -lhfsss-common -lm $(LDFLAGS)

$(TEST_SECURITY): $(TEST_DIR)/test_security.c $(LIBHFSSS_CTRL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-controller -lhfsss-common $(LDFLAGS)

$(TEST_MULTI_NS): $(TEST_DIR)/test_multi_ns.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common -lm $(LDFLAGS)

$(TEST_THERMAL_TEL): $(TEST_DIR)/test_thermal_telemetry.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common -lm $(LDFLAGS)

$(STRESS_ENTERPRISE): $(TEST_DIR)/stress_enterprise.c $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

$(TEST_PROC): $(TEST_DIR)/test_proc_interface.c $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-common -lm $(LDFLAGS)

$(STRESS_STABILITY): $(TEST_DIR)/stress_stability.c $(LIBHFSSS_FTL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_HAL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-media -lhfsss-hal -lhfsss-common -lm $(LDFLAGS)

$(TEST_LARGE_CAP): $(TEST_DIR)/test_large_capacity.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

# vhost library objects
$(BUILD_DIR)/vhost/%.o: $(SRC_DIR)/vhost/%.c
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) -c $< -o $@

# vhost-user-blk server executable (WIP — re-enable when source is committed)
# $(HFSSS_VHOST): $(VHOST_SRC)/hfsss_vhost_main.c $(VHOST_OBJS) $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
# 	@echo "  CC      $@"
# 	@$(CC) $(CFLAGS) $(VHOST_SRC)/hfsss_vhost_main.c $(VHOST_OBJS) -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

# hfsss-img-export (raw disk image exporter for QEMU)
$(HFSSS_IMG_EXPORT): $(VHOST_SRC)/hfsss_img_export.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

# hfsss-nbd-server (NBD server exposing simulator as live block device for QEMU)
$(HFSSS_NBD): $(VHOST_SRC)/hfsss_nbd_server.c $(VHOST_SRC)/nbd_async.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $(VHOST_SRC)/hfsss_nbd_server.c $(VHOST_SRC)/nbd_async.c -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

# vhost protocol unit tests (WIP — depends on uncommitted vhost_user_blk source)
# $(TEST_VHOST): $(TEST_DIR)/test_vhost_proto.c $(VHOST_OBJS) $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
# 	@echo "  CC      $@"
# 	@$(CC) $(CFLAGS) $< $(VHOST_OBJS) -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common -lm $(LDFLAGS)

stress-long: all
	@echo "Running stability stress test (duration=$(or $(STRESS_DURATION),60)s)..."
	@STRESS_DURATION=$(or $(STRESS_DURATION),60) $(STRESS_STABILITY)

# System-level tests (Tier 1)
.PHONY: systest
systest: directories $(SYSTEST_DI) $(SYSTEST_NC) $(SYSTEST_EB)
	@echo "========================================"
	@echo "Running system-level tests..."
	@echo "========================================"
	@$(SYSTEST_DI)
	@echo ""
	@$(SYSTEST_NC)
	@echo ""
	@$(SYSTEST_EB)
	@echo ""
	@echo "========================================"
	@echo "All system-level tests complete!"
	@echo "========================================"

# Test
test: all
	@echo "========================================"
	@echo "Running tests..."
	@echo "========================================"
	@$(TEST_COMMON)
	@echo ""
	@$(TEST_MEDIA)
	@echo ""
	@$(TEST_HAL)
	@echo ""
	@$(TEST_FTL)
	@echo ""
	@$(TEST_CTRL)
	@echo ""
	@$(TEST_SHMEM_IF)
	@echo ""
	@$(TEST_MSGQ)
	@echo ""
	@$(TEST_PCIE)
	@echo ""
	@$(TEST_NVME_ADMIN)
	@echo ""
	@$(TEST_NVME_IO)
	@echo ""
	@$(TEST_SSSIM)
	@echo ""
	@$(TEST_NVME_USPACE)
	@echo ""
	@$(TEST_BOOT)
	@echo ""
	@$(TEST_NOR)
	@echo ""
	@$(TEST_FTL_REL)
	@echo ""
	@$(TEST_RT)
	@echo ""
	@$(TEST_OOB)
	@echo ""
	@$(TEST_CONFIG)
	@echo ""
	@$(TEST_FAULT)
	@echo ""
	@$(TEST_RELIABILITY)
	@echo ""
	@$(TEST_PERF)
	@echo ""
	@$(TEST_DSM)
	@echo ""
	@$(TEST_PRP)
	@echo ""
	@$(TEST_FTL_INT)
	@echo ""
	@$(TEST_POWER_CYCLE)
	@echo ""
	@$(TEST_FOUNDATION)
	@echo ""
	@$(TEST_T10PI)
	@echo ""
	@$(TEST_UPLP)
	@echo ""
	@$(TEST_QOS)
	@echo ""
	@$(TEST_SECURITY)
	@echo ""
	@$(TEST_MULTI_NS)
	@echo ""
	@$(TEST_THERMAL_TEL)
	@echo ""
	@$(TEST_PROC)
	@echo ""
	@echo ""

# Clean
clean:
	@echo "  CLEAN   build/"
	@rm -rf $(BUILD_DIR)
	@echo "Clean complete!"

# Help
help:
	@echo "HFSSS Build System"
	@echo "=================="
	@echo ""
	@echo "Build targets:"
	@echo "  all                - Build all libraries and binaries (-O2)"
	@echo "  test               - Run unit tests"
	@echo "  systest            - Run system-level tests"
	@echo "  stress-long        - Run extended stability stress test"
	@echo "  clean              - Clean build/ directory"
	@echo "  help               - Show this help message"
	@echo ""
	@echo "Coverage targets (gcov + lcov, see docs/coverage.md):"
	@echo "  coverage-build     - Build instrumented binaries (--coverage -O0) into build-cov/"
	@echo "  coverage-ut        - Run UT and generate HTML report"
	@echo "  coverage-e2e       - Run E2E tests and generate HTML report"
	@echo "  coverage-merge     - Merge UT and E2E reports into a single report"
	@echo "  coverage           - Run the full coverage flow (UT, E2E, and merge)"
	@echo "  coverage-selftest  - Run all 5 coverage infra self-tests"
	@echo "  coverage-clean     - Clean build-cov/ + remove stale .gcda files"
	@echo ""
	@echo "QEMU black-box targets:"
	@echo "  qemu-blackbox-list - List guest-visible QEMU/NVMe black-box cases"
	@echo "  qemu-blackbox      - Run the QEMU/NVMe black-box suite"
	@echo "  qemu-blackbox-ci   - Run the black-box suite with stable CI artifact paths"
	@echo "  qemu-blackbox-soak - Run repeated black-box rounds in one isolated env"
	@echo ""
	@echo "Pre-checkin gate (MANDATORY before every commit/PR):"
	@echo "  pre-checkin        - Run the full blackbox bundle w/ CRC32C verify (~8-12 min)"
	@echo "                       See docs/PRE_CHECKIN_STANDARD.md"
	@echo ""
	@echo "Examples:"
	@echo "  make all           - Build everything"
	@echo "  make test          - Build and run tests"
	@echo "  make coverage-ut   - Build instrumented + run UT + open build-cov/coverage/ut/index.html"
	@echo "  make qemu-blackbox BLACKBOX_ARGS=\"--guest-dir /path/to/guest --case 001_nvme_cli_smoke.sh\""
	@echo "  make qemu-blackbox-ci BLACKBOX_ARGS=\"--guest-dir /path/to/guest --skip-build\""
	@echo "  make qemu-blackbox-soak BLACKBOX_ARGS=\"--guest-dir /path/to/guest --rounds 10\""
	@echo "  make pre-checkin   - MANDATORY gate before committing (see CONTRIBUTING.md)"
	@echo "  make clean         - Clean up"

# Coverage targets
coverage-build:
	@bash scripts/coverage/check_target_set.sh
	@$(MAKE) COVERAGE=1 directories $(COVERAGE_BINS)

coverage-clean:
	@echo "  CLEAN   build-cov/"
	@rm -rf build-cov
	@find . -name '*.gcda' -delete 2>/dev/null || true
	@echo "Coverage clean complete!"

coverage-ut: coverage-build
	@bash scripts/coverage/run_ut_coverage.sh

coverage-e2e: coverage-build
	@bash scripts/coverage/run_e2e_coverage.sh

coverage-merge:
	@bash scripts/coverage/merge_reports.sh

# Full local flow: UT + E2E + merge
coverage: coverage-ut coverage-e2e coverage-merge

coverage-selftest: coverage-ut coverage-e2e
	@echo "Running coverage self-tests..."
	@bash scripts/coverage/tests/test_build.sh
	@bash scripts/coverage/tests/test_reset.sh
	@bash scripts/coverage/tests/test_exclusion.sh
	@bash scripts/coverage/tests/test_merge.sh
	@bash scripts/coverage/tests/test_ratchet.sh
	@echo "All coverage self-tests passed!"

qemu-blackbox-list:
	@./scripts/run_qemu_blackbox_tests.sh --list

qemu-blackbox:
	@./scripts/run_qemu_blackbox_tests.sh $(BLACKBOX_ARGS)

qemu-blackbox-ci:
	@./scripts/run_qemu_blackbox_ci.sh $(BLACKBOX_ARGS)

qemu-blackbox-soak:
	@./scripts/run_qemu_blackbox_soak.sh $(BLACKBOX_ARGS)

# --------------------------------------------------------------------------
# pre-checkin: mandatory end-to-end gate for every PR and every agent.
# See docs/PRE_CHECKIN_STANDARD.md and CONTRIBUTING.md for the full policy.
# Runs the full blackbox bundle (nvme-cli smokes + fio read/write/trim +
# the 014 mixed-rw stress case with CRC32C verification) through QEMU + NBD.
# Expects guest assets under $(GUEST_DIR) or explicit BLACKBOX_ARGS override.
# --------------------------------------------------------------------------
GUEST_DIR ?= $(CURDIR)/guest
pre-checkin:
	@echo "========================================"
	@echo "  HFSSS pre-checkin gate"
	@echo "  Bundle: 001,002,003 nvme-cli + 010,011,012,013 fio + 014 stress + 900 spdk"
	@echo "  Guest:  $(GUEST_DIR)"
	@echo "========================================"
	@ok=1; \
	for f in "$(GUEST_DIR)/alpine-hfsss.qcow2" \
	         "$(GUEST_DIR)/cidata.iso" \
	         "$(GUEST_DIR)/ovmf_vars-saved.fd" \
	         "$${HFSSS_QEMU_CODE_FD:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}"; do \
		if [ ! -f "$$f" ]; then \
			echo "ERROR: required asset missing: $$f"; \
			ok=0; \
		fi; \
	done; \
	if [ $$ok -eq 0 ]; then \
		echo "       See docs/PRE_CHECKIN_STANDARD.md for setup. No skipping allowed."; \
		echo "       Override UEFI code path with HFSSS_QEMU_CODE_FD=/path/to/edk2-aarch64-code.fd"; \
		exit 1; \
	fi
	@HFSSS_CASE_TIMEOUT_S=1800 ./scripts/run_qemu_blackbox_ci.sh --guest-dir "$(GUEST_DIR)" \
		--size-mb 2048 --mode async \
		--case 001_nvme_cli_smoke.sh \
		--case 002_nvme_namespace_info.sh \
		--case 003_nvme_flush_smoke.sh \
		--case 010_fio_randwrite_verify.sh \
		--case 011_fio_randrw_verify.sh \
		--case 012_fio_seqwrite_verify.sh \
		--case 013_fio_trim_verify.sh \
		--case 014_fio_pre_checkin_stress.sh \
		--case 900_spdk_nvme_identify.sh \
		$(BLACKBOX_ARGS)
	@echo "========================================"
	@echo "  pre-checkin PASS — ready to commit"
	@echo "========================================"
