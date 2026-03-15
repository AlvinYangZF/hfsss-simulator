# HFSSS - High Fidelity Full-Stack SSD Simulator Makefile

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -std=gnu11 -fPIC
CFLAGS += -Iinclude -Iinclude/common -Iinclude/media -Iinclude/hal -Iinclude/ftl -Iinclude/controller -Iinclude/pcie
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lpthread -lrt
else
    LDFLAGS = -lpthread
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
BUILD_DIR = build
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
TEST_PCIE = $(BIN_DIR)/test_pcie_nvme
TEST_SSSIM = $(BIN_DIR)/test_sssim
TEST_NVME_USPACE = $(BIN_DIR)/test_nvme_uspace

# Targets
.PHONY: all clean directories test help

all: directories $(LIBHFSSS_COMMON) $(LIBHFSSS_MEDIA) $(LIBHFSSS_HAL) $(LIBHFSSS_FTL) $(LIBHFSSS_CTRL) $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(TEST_COMMON) $(TEST_MEDIA) $(TEST_HAL) $(TEST_FTL) $(TEST_CTRL) $(TEST_PCIE) $(TEST_SSSIM) $(TEST_NVME_USPACE)
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
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-sssim -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_CTRL): $(TEST_DIR)/test_controller.c $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_PCIE): $(TEST_DIR)/test_pcie_nvme.c $(LIBHFSSS_PCIE) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

$(TEST_NVME_USPACE): $(TEST_DIR)/test_nvme_uspace.c $(LIBHFSSS_PCIE) $(LIBHFSSS_SSSIM) $(LIBHFSSS_CTRL) $(LIBHFSSS_FTL) $(LIBHFSSS_HAL) $(LIBHFSSS_MEDIA) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-pcie -lhfsss-sssim -lhfsss-controller -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common $(LDFLAGS)

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
	@$(TEST_PCIE)
	@echo ""
	@$(TEST_SSSIM)
	@echo ""
	@$(TEST_NVME_USPACE)

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
	@echo "Targets:"
	@echo "  all         - Build all libraries"
	@echo "  test        - Run tests"
	@echo "  clean       - Clean build directory"
	@echo "  help        - Show this help message"
	@echo ""
	@echo "Example:"
	@echo "  make all    - Build everything"
	@echo "  make test   - Build and run tests"
	@echo "  make clean  - Clean up"
