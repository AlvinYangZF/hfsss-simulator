# HFSSS User Guide (English)

**Version**: 1.0

---

## Table of Contents
1. [Quick Start](#quick-start)
2. [Basic Usage](#basic-usage)
3. [Configuration](#configuration)
4. [Statistics](#statistics)
5. [Complete Example Programs](#complete-example-programs)

---

## Quick Start

### Building the Simulator

```bash
cd /path/to/hfsss-simulator
make all
```

### Running Tests

```bash
make test
```

---

## Basic Usage

### Including the Header

```c
#include "sssim.h"
```

### Initializing the Simulator

```c
#include <stdio.h>
#include <string.h>
#include "sssim.h"

int main(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    int ret;

    /* Initialize with default configuration */
    sssim_config_default(&config);

    /* Optional: Customize configuration */
    config.total_lbas = 1024;  /* Small 4MB SSD for testing */

    /* Initialize the SSD simulator */
    ret = sssim_init(&ctx, &config);
    if (ret != HFSSS_OK) {
        printf("Failed to initialize SSD: %d\n", ret);
        return 1;
    }

    /* Use the SSD... */

    /* Cleanup when done */
    sssim_cleanup(&ctx);

    return 0;
}
```

### Writing Data

```c
u8 write_buf[4096];  /* 1 page */

/* Fill buffer with test pattern */
memset(write_buf, 0xAA, 4096);

/* Write 1 LBA starting at LBA 0 */
ret = sssim_write(&ctx, 0, 1, write_buf);
if (ret != HFSSS_OK) {
    printf("Write failed: %d\n", ret);
}
```

### Reading Data

```c
u8 read_buf[4096];

/* Read 1 LBA starting at LBA 0 */
ret = sssim_read(&ctx, 0, 1, read_buf);
if (ret != HFSSS_OK) {
    printf("Read failed: %d\n", ret);
}

/* Verify data */
if (memcmp(write_buf, read_buf, 4096) == 0) {
    printf("Data matches!\n");
} else {
    printf("Data mismatch!\n");
}
```

### Trimming Data (TRIM/DISCARD)

```c
/* Trim 4 LBAs starting at LBA 100 */
ret = sssim_trim(&ctx, 100, 4);
if (ret != HFSSS_OK) {
    printf("Trim failed: %d\n", ret);
}
```

### Flushing Writes

```c
/* Flush all pending writes to media */
ret = sssim_flush(&ctx);
if (ret != HFSSS_OK) {
    printf("Flush failed: %d\n", ret);
}
```

---

## Configuration

### Default Configuration

The default configuration creates a 1GB SSD:

```c
struct sssim_config config;
sssim_config_default(&config);
```

Default values:
- `total_lbas`: 262,144 (1GB with 4KB LBA)
- `lba_size`: 4096 bytes
- `page_size`: 4096 bytes
- `spare_size`: 128 bytes
- `pages_per_block`: 256
- `blocks_per_plane`: 1024
- `planes_per_die`: 2
- `dies_per_chip`: 2
- `chips_per_channel`: 2
- `channel_count`: 2
- `op_ratio`: 10 (10% over-provisioning)
- `gc_policy`: GC_POLICY_GREEDY
- `nand_type`: NAND_TYPE_TLC

### Custom Configuration Example

```c
struct sssim_config config;

/* Start with defaults */
sssim_config_default(&config);

/* Create a smaller 64MB SSD */
config.total_lbas = 16384;  /* 16384 * 4KB = 64MB */

/* Use SLC NAND instead of TLC */
config.nand_type = NAND_TYPE_SLC;

/* Change GC policy */
config.gc_policy = GC_POLICY_COST_BENEFIT;

/* Adjust over-provisioning */
config.op_ratio = 20;  /* 20% OP */
```

---

## Statistics

### Getting FTL Statistics

```c
struct ftl_stats stats;

/* Get statistics */
sssim_get_stats(&ctx, &stats);

/* Print statistics */
printf("=== FTL Statistics ===\n");
printf("Write count:     %llu\n", (unsigned long long)stats.write_count);
printf("Read count:      %llu\n", (unsigned long long)stats.read_count);
printf("Trim count:      %llu\n", (unsigned long long)stats.trim_count);
printf("Write bytes:     %llu\n", (unsigned long long)stats.write_bytes);
printf("Read bytes:      %llu\n", (unsigned long long)stats.read_bytes);
printf("GC count:        %llu\n", (unsigned long long)stats.gc_count);
printf("Moved pages:     %llu\n", (unsigned long long)stats.moved_pages);
printf("Reclaimed blocks:%llu\n", (unsigned long long)stats.reclaimed_blocks);
```

### Resetting Statistics

```c
sssim_reset_stats(&ctx);
```

---

## Complete Example Programs

### Example 1: Simple Read/Write Test

```c
#include <stdio.h>
#include <string.h>
#include "sssim.h"

int main(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 write_buf[4096];
    u8 read_buf[4096];
    struct ftl_stats stats;
    int ret;
    int i;

    printf("=== HFSSS Simple Read/Write Test ===\n\n");

    /* Initialize with default configuration */
    sssim_config_default(&config);
    config.total_lbas = 1024;  /* Small 4MB SSD */

    ret = sssim_init(&ctx, &config);
    if (ret != HFSSS_OK) {
        printf("sssim_init failed: %d\n", ret);
        return 1;
    }
    printf("SSD initialized successfully\n\n");

    /* Write some data */
    printf("Writing data...\n");
    for (i = 0; i < 4; i++) {
        memset(write_buf, 0xAA + i, 4096);
        ret = sssim_write(&ctx, i * 10, 1, write_buf);
        if (ret != HFSSS_OK) {
            printf("Write failed at LBA %d: %d\n", i * 10, ret);
        }
    }
    printf("Writes complete\n\n");

    /* Read and verify */
    printf("Reading and verifying data...\n");
    for (i = 0; i < 4; i++) {
        memset(read_buf, 0, 4096);
        ret = sssim_read(&ctx, i * 10, 1, read_buf);
        if (ret != HFSSS_OK) {
            printf("Read failed at LBA %d: %d\n", i * 10, ret);
            continue;
        }

        memset(write_buf, 0xAA + i, 4096);
        if (memcmp(write_buf, read_buf, 4096) == 0) {
            printf("  LBA %d: OK\n", i * 10);
        } else {
            printf("  LBA %d: DATA MISMATCH\n", i * 10);
        }
    }
    printf("\n");

    /* Get and print stats */
    sssim_get_stats(&ctx, &stats);
    printf("=== Statistics ===\n");
    printf("Writes: %llu\n", (unsigned long long)stats.write_count);
    printf("Reads:  %llu\n", (unsigned long long)stats.read_count);
    printf("\n");

    /* Cleanup */
    sssim_cleanup(&ctx);
    printf("Test complete\n");

    return 0;
}
```

### Example 2: Compile and Run

To compile your own program using HFSSS:

```bash
# First build the HFSSS libraries
make all

# Compile your program
gcc -Wall -Wextra -g -Iinclude -c my_program.c -o my_program.o

# Link with HFSSS libraries
gcc my_program.o -o my_program -Lbuild/lib \
    -lhfsss-sssim -lhfsss-ftl -lhfsss-hal -lhfsss-media -lhfsss-common \
    -lpthread

# Run
./my_program
```

### Example 3: Sequential Write Performance Test

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sssim.h"

#define TEST_LBAS 1000

int main(void)
{
    struct sssim_ctx ctx;
    struct sssim_config config;
    u8 *buf;
    struct timespec start, end;
    double elapsed_sec;
    double mb_per_sec;
    int ret;
    int i;

    printf("=== HFSSS Sequential Write Test ===\n\n");

    /* Allocate buffer */
    buf = malloc(4096);
    if (!buf) {
        printf("malloc failed\n");
        return 1;
    }
    memset(buf, 0x55, 4096);

    /* Initialize SSD */
    sssim_config_default(&config);
    config.total_lbas = TEST_LBAS + 100;

    ret = sssim_init(&ctx, &config);
    if (ret != HFSSS_OK) {
        printf("sssim_init failed: %d\n", ret);
        free(buf);
        return 1;
    }

    /* Perform sequential writes */
    printf("Writing %d LBAs...\n", TEST_LBAS);
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (i = 0; i < TEST_LBAS; i++) {
        ret = sssim_write(&ctx, i, 1, buf);
        if (ret != HFSSS_OK) {
            printf("Write failed at LBA %d: %d\n", i, ret);
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    /* Calculate throughput */
    elapsed_sec = (end.tv_sec - start.tv_sec) +
                  (end.tv_nsec - start.tv_nsec) / 1e9;
    mb_per_sec = (TEST_LBAS * 4.0 / 1024.0) / elapsed_sec;

    printf("\n=== Results ===\n");
    printf("Time: %.3f seconds\n", elapsed_sec);
    printf("Throughput: %.2f MB/s\n", mb_per_sec);
    printf("IOPS: %.0f\n", TEST_LBAS / elapsed_sec);

    /* Cleanup */
    sssim_cleanup(&ctx);
    free(buf);

    return 0;
}
```

---

## Error Codes

HFSSS uses the following error codes:

| Code | Value | Description |
|------|-------|-------------|
| HFSSS_OK | 0 | Success |
| HFSSS_ERR_INVAL | -1 | Invalid parameter |
| HFSSS_ERR_NOMEM | -2 | Out of memory |
| HFSSS_ERR_NOENT | -3 | Entry not found |
| HFSSS_ERR_BUSY | -4 | Resource busy |
| HFSSS_ERR_IO | -5 | I/O error |
| HFSSS_ERR_NOSPC | -6 | No space left |

---

## Next Steps

- Check the [Architecture Overview](ARCHITECTURE.md) for detailed module descriptions
- Look at the test files in the `tests/` directory for more examples
- Read the Chinese design documents in `docs/` for the original full design
