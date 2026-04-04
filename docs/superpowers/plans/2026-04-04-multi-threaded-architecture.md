# Multi-Threaded HFSSS Architecture — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform HFSSS from single-threaded (~3 MB/s) to 7-thread Marvell SC5-style architecture (target ~4x throughput) with TAA-sharded L2P, lock-free I/O queues, 4 FTL workers, background GC thread, and WL/RD thread.

**Architecture:** NBD dispatch thread routes I/O through SPSC lock-free ring buffers to 4 fixed FTL worker threads. Each worker owns a channel and accesses the L2P mapping through TAA (Table Access Accelerator) — a software approximation of Marvell's hardware TAA using per-LBA-range sharded mutexes. GC and wear-leveling run in dedicated background threads signaled by condition variables.

**Tech Stack:** C11 (gcc), pthreads, atomic builtins (`__atomic_*`), existing project test framework (TEST_ASSERT macro)

**Target repo:** `/Users/zifengyang/Desktop/hfsss-simulator`

---

## File Map

| Action | Path | Responsibility |
|--------|------|---------------|
| Create | `include/ftl/taa.h` | TAA shard structure, API |
| Create | `src/ftl/taa.c` | TAA init/cleanup/lookup/update/stats |
| Create | `include/ftl/io_queue.h` | Lock-free SPSC ring buffer for I/O dispatch |
| Create | `src/ftl/io_queue.c` | Ring buffer push/pop/is_empty |
| Create | `include/ftl/ftl_worker.h` | FTL worker thread context + API |
| Create | `src/ftl/ftl_worker.c` | Worker thread main loop, per-channel I/O |
| Create | `src/ftl/gc_thread.c` | Background GC thread with condition variable wake |
| Create | `src/ftl/wl_thread.c` | Background wear-leveling + read disturb thread |
| Modify | `src/ftl/ftl.c` | Add `ftl_mt_*` functions that dispatch through workers |
| Modify | `include/ftl/ftl.h` | Add multi-thread fields to `ftl_ctx` |
| Modify | `src/vhost/hfsss_nbd_server.c` | Async dispatch + completion draining |
| Modify | `Makefile` | New source files, test targets |
| Create | `tests/test_taa.c` | TAA unit tests (shard lookup, concurrent access) |
| Create | `tests/test_io_queue.c` | SPSC/MPSC ring buffer unit tests |
| Create | `tests/test_mt_ftl.c` | Multi-threaded FTL integration test |

---

### Task 1: TAA (Table Access Accelerator) — Header + Implementation

**Files:**
- Create: `include/ftl/taa.h`
- Create: `src/ftl/taa.c`

- [ ] **Step 1: Create TAA header**

Create `include/ftl/taa.h`:

```c
#ifndef __HFSSS_TAA_H
#define __HFSSS_TAA_H

#include "common/common.h"
#include "common/mutex.h"
#include "ftl/mapping.h"

#define TAA_DEFAULT_SHARDS 256

/* TAA Shard — owns a range of LBAs with its own lock */
struct taa_shard {
    struct l2p_entry *l2p;
    struct p2l_entry *p2l;
    struct mutex      lock;
    u64               base_lba;
    u64               lba_count;
    u64               p2l_base;
    u64               p2l_count;
    u64               valid_count;
    u64               lookup_count;
    u64               conflict_count;
};

/* TAA Context — replaces single mapping_ctx for multi-threaded access */
struct taa_ctx {
    u32               num_shards;
    u64               total_lbas;
    u64               total_pages;
    u64               lbas_per_shard;
    struct taa_shard *shards;
    bool              initialized;
};

/* Lifecycle */
int  taa_init(struct taa_ctx *ctx, u64 total_lbas, u64 total_pages,
              u32 num_shards);
void taa_cleanup(struct taa_ctx *ctx);

/* L2P operations — each locks only the relevant shard */
int  taa_lookup(struct taa_ctx *ctx, u64 lba, union ppn *ppn_out);
int  taa_insert(struct taa_ctx *ctx, u64 lba, union ppn ppn);
int  taa_remove(struct taa_ctx *ctx, u64 lba);
int  taa_update(struct taa_ctx *ctx, u64 lba, union ppn new_ppn,
                union ppn *old_ppn);

/* P2L reverse lookup */
int  taa_reverse_lookup(struct taa_ctx *ctx, union ppn ppn, u64 *lba_out);

/* Bulk operations for recovery */
int  taa_direct_set(struct taa_ctx *ctx, u64 lba, union ppn ppn);
int  taa_direct_clear(struct taa_ctx *ctx, u64 lba);
int  taa_rebuild_p2l(struct taa_ctx *ctx);

/* Statistics */
u64  taa_get_valid_count(struct taa_ctx *ctx);
void taa_get_stats(struct taa_ctx *ctx, u64 *total_lookups,
                   u64 *total_conflicts);

#endif /* __HFSSS_TAA_H */
```

- [ ] **Step 2: Implement TAA**

Create `src/ftl/taa.c`:

```c
#include "ftl/taa.h"
#include <stdlib.h>
#include <string.h>

static inline u32 taa_shard_id(struct taa_ctx *ctx, u64 lba)
{
    return (u32)(lba / ctx->lbas_per_shard);
}

static inline u64 p2l_idx(union ppn ppn, u64 table_size)
{
    return (ppn.raw ^ (ppn.raw >> 24)) % table_size;
}

int taa_init(struct taa_ctx *ctx, u64 total_lbas, u64 total_pages,
             u32 num_shards)
{
    u32 i;

    if (!ctx || total_lbas == 0 || num_shards == 0) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->num_shards = num_shards;
    ctx->total_lbas = total_lbas;
    ctx->total_pages = total_pages;
    ctx->lbas_per_shard = (total_lbas + num_shards - 1) / num_shards;

    ctx->shards = (struct taa_shard *)calloc(num_shards,
                                              sizeof(struct taa_shard));
    if (!ctx->shards) {
        return HFSSS_ERR_NOMEM;
    }

    u64 p2l_per_shard = (total_pages + num_shards - 1) / num_shards;

    for (i = 0; i < num_shards; i++) {
        struct taa_shard *s = &ctx->shards[i];
        int ret = mutex_init(&s->lock);
        if (ret != HFSSS_OK) {
            goto fail;
        }

        s->base_lba = (u64)i * ctx->lbas_per_shard;
        s->lba_count = ctx->lbas_per_shard;
        if (s->base_lba + s->lba_count > total_lbas) {
            s->lba_count = total_lbas - s->base_lba;
        }

        s->p2l_base = (u64)i * p2l_per_shard;
        s->p2l_count = p2l_per_shard;

        s->l2p = (struct l2p_entry *)calloc(s->lba_count,
                                             sizeof(struct l2p_entry));
        if (!s->l2p) {
            goto fail;
        }

        s->p2l = (struct p2l_entry *)calloc(s->p2l_count,
                                             sizeof(struct p2l_entry));
        if (!s->p2l) {
            goto fail;
        }
    }

    ctx->initialized = true;
    return HFSSS_OK;

fail:
    taa_cleanup(ctx);
    return HFSSS_ERR_NOMEM;
}

void taa_cleanup(struct taa_ctx *ctx)
{
    u32 i;
    if (!ctx || !ctx->shards) {
        return;
    }
    for (i = 0; i < ctx->num_shards; i++) {
        free(ctx->shards[i].l2p);
        free(ctx->shards[i].p2l);
        mutex_cleanup(&ctx->shards[i].lock);
    }
    free(ctx->shards);
    memset(ctx, 0, sizeof(*ctx));
}

int taa_lookup(struct taa_ctx *ctx, u64 lba, union ppn *ppn_out)
{
    if (!ctx || !ctx->initialized || !ppn_out) {
        return HFSSS_ERR_INVAL;
    }
    if (lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);
    s->lookup_count++;

    if (!s->l2p[local_lba].valid) {
        mutex_unlock(&s->lock);
        return HFSSS_ERR_NOENT;
    }

    *ppn_out = s->l2p[local_lba].ppn;
    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_insert(struct taa_ctx *ctx, u64 lba, union ppn ppn)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    s->l2p[local_lba].ppn = ppn;
    s->l2p[local_lba].valid = true;

    u64 idx = p2l_idx(ppn, s->p2l_count);
    s->p2l[idx].lba = lba;
    s->p2l[idx].valid = true;
    s->valid_count++;

    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_remove(struct taa_ctx *ctx, u64 lba)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    if (!s->l2p[local_lba].valid) {
        mutex_unlock(&s->lock);
        return HFSSS_ERR_NOENT;
    }

    union ppn ppn = s->l2p[local_lba].ppn;
    s->l2p[local_lba].valid = false;
    memset(&s->l2p[local_lba].ppn, 0, sizeof(union ppn));

    u64 idx = p2l_idx(ppn, s->p2l_count);
    s->p2l[idx].valid = false;
    s->p2l[idx].lba = 0;
    s->valid_count--;

    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_update(struct taa_ctx *ctx, u64 lba, union ppn new_ppn,
               union ppn *old_ppn)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }

    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;

    mutex_lock(&s->lock, 0);

    if (old_ppn && s->l2p[local_lba].valid) {
        *old_ppn = s->l2p[local_lba].ppn;
    }

    /* Remove old P2L entry */
    if (s->l2p[local_lba].valid) {
        union ppn old = s->l2p[local_lba].ppn;
        u64 old_idx = p2l_idx(old, s->p2l_count);
        s->p2l[old_idx].valid = false;
        s->valid_count--;
    }

    /* Insert new mapping */
    s->l2p[local_lba].ppn = new_ppn;
    s->l2p[local_lba].valid = true;

    u64 new_idx = p2l_idx(new_ppn, s->p2l_count);
    s->p2l[new_idx].lba = lba;
    s->p2l[new_idx].valid = true;
    s->valid_count++;

    mutex_unlock(&s->lock);
    return HFSSS_OK;
}

int taa_reverse_lookup(struct taa_ctx *ctx, union ppn ppn, u64 *lba_out)
{
    if (!ctx || !ctx->initialized || !lba_out) {
        return HFSSS_ERR_INVAL;
    }

    /* P2L entries are distributed across shards — scan all shards */
    for (u32 i = 0; i < ctx->num_shards; i++) {
        struct taa_shard *s = &ctx->shards[i];
        u64 idx = p2l_idx(ppn, s->p2l_count);

        mutex_lock(&s->lock, 0);
        if (s->p2l[idx].valid) {
            *lba_out = s->p2l[idx].lba;
            mutex_unlock(&s->lock);
            return HFSSS_OK;
        }
        mutex_unlock(&s->lock);
    }
    return HFSSS_ERR_NOENT;
}

int taa_direct_set(struct taa_ctx *ctx, u64 lba, union ppn ppn)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }
    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;
    s->l2p[local_lba].ppn = ppn;
    s->l2p[local_lba].valid = true;
    return HFSSS_OK;
}

int taa_direct_clear(struct taa_ctx *ctx, u64 lba)
{
    if (!ctx || !ctx->initialized || lba >= ctx->total_lbas) {
        return HFSSS_ERR_INVAL;
    }
    u32 sid = taa_shard_id(ctx, lba);
    struct taa_shard *s = &ctx->shards[sid];
    u64 local_lba = lba - s->base_lba;
    s->l2p[local_lba].valid = false;
    memset(&s->l2p[local_lba].ppn, 0, sizeof(union ppn));
    return HFSSS_OK;
}

int taa_rebuild_p2l(struct taa_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    for (u32 i = 0; i < ctx->num_shards; i++) {
        struct taa_shard *s = &ctx->shards[i];
        mutex_lock(&s->lock, 0);

        memset(s->p2l, 0, s->p2l_count * sizeof(struct p2l_entry));
        s->valid_count = 0;

        for (u64 j = 0; j < s->lba_count; j++) {
            if (s->l2p[j].valid) {
                union ppn ppn = s->l2p[j].ppn;
                u64 idx = p2l_idx(ppn, s->p2l_count);
                s->p2l[idx].lba = s->base_lba + j;
                s->p2l[idx].valid = true;
                s->valid_count++;
            }
        }

        mutex_unlock(&s->lock);
    }
    return HFSSS_OK;
}

u64 taa_get_valid_count(struct taa_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return 0;
    }
    u64 total = 0;
    for (u32 i = 0; i < ctx->num_shards; i++) {
        total += ctx->shards[i].valid_count;
    }
    return total;
}

void taa_get_stats(struct taa_ctx *ctx, u64 *total_lookups,
                   u64 *total_conflicts)
{
    if (!ctx || !ctx->initialized) {
        return;
    }
    u64 lookups = 0, conflicts = 0;
    for (u32 i = 0; i < ctx->num_shards; i++) {
        lookups += ctx->shards[i].lookup_count;
        conflicts += ctx->shards[i].conflict_count;
    }
    if (total_lookups) *total_lookups = lookups;
    if (total_conflicts) *total_conflicts = conflicts;
}
```

- [ ] **Step 3: Build**

```bash
cd /Users/zifengyang/Desktop/hfsss-simulator
# Add taa.c to Makefile FTL sources (after mapping.o)
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/ftl/taa.h src/ftl/taa.c Makefile
git commit -m "feat: add TAA (Table Access Accelerator) — sharded L2P/P2L mapping"
```

---

### Task 2: TAA Unit Tests

**Files:**
- Create: `tests/test_taa.c`
- Modify: `Makefile`

- [ ] **Step 1: Write TAA test**

Create `tests/test_taa.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <inttypes.h>
#include "ftl/taa.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void test_init_cleanup(void)
{
    printf("\n=== TAA Init/Cleanup ===\n");

    struct taa_ctx ctx;
    int ret = taa_init(&ctx, 1024, 2048, 4);
    TEST_ASSERT(ret == HFSSS_OK, "taa_init succeeds");
    TEST_ASSERT(ctx.num_shards == 4, "4 shards");
    TEST_ASSERT(ctx.lbas_per_shard == 256, "256 LBAs per shard");

    taa_cleanup(&ctx);
    TEST_ASSERT(ctx.initialized == false, "cleanup resets initialized");

    /* NULL safety */
    ret = taa_init(NULL, 1024, 2048, 4);
    TEST_ASSERT(ret != HFSSS_OK, "init(NULL) fails");
    taa_cleanup(NULL);
    TEST_ASSERT(1, "cleanup(NULL) does not crash");
}

static void test_lookup_insert_remove(void)
{
    printf("\n=== TAA Lookup/Insert/Remove ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    union ppn ppn;
    ppn.raw = 0;
    ppn.bits.channel = 1;
    ppn.bits.block = 10;
    ppn.bits.page = 5;

    /* Insert */
    int ret = taa_insert(&ctx, 42, ppn);
    TEST_ASSERT(ret == HFSSS_OK, "insert LBA 42");

    /* Lookup */
    union ppn out;
    ret = taa_lookup(&ctx, 42, &out);
    TEST_ASSERT(ret == HFSSS_OK, "lookup LBA 42 succeeds");
    TEST_ASSERT(out.raw == ppn.raw, "lookup returns correct PPN");

    /* Lookup unmapped */
    ret = taa_lookup(&ctx, 999, &out);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "lookup unmapped returns NOENT");

    /* Remove */
    ret = taa_remove(&ctx, 42);
    TEST_ASSERT(ret == HFSSS_OK, "remove LBA 42");
    ret = taa_lookup(&ctx, 42, &out);
    TEST_ASSERT(ret == HFSSS_ERR_NOENT, "removed LBA returns NOENT");

    /* Valid count */
    TEST_ASSERT(taa_get_valid_count(&ctx) == 0, "valid count is 0");

    taa_cleanup(&ctx);
}

static void test_update(void)
{
    printf("\n=== TAA Update ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    union ppn ppn1, ppn2, old;
    ppn1.raw = 0x100;
    ppn2.raw = 0x200;

    taa_insert(&ctx, 50, ppn1);

    int ret = taa_update(&ctx, 50, ppn2, &old);
    TEST_ASSERT(ret == HFSSS_OK, "update succeeds");
    TEST_ASSERT(old.raw == ppn1.raw, "old PPN returned");

    union ppn out;
    taa_lookup(&ctx, 50, &out);
    TEST_ASSERT(out.raw == ppn2.raw, "lookup returns new PPN");

    TEST_ASSERT(taa_get_valid_count(&ctx) == 1, "valid count is 1");

    taa_cleanup(&ctx);
}

static void test_cross_shard(void)
{
    printf("\n=== TAA Cross-Shard ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    /* Insert into different shards (shard = lba / 256) */
    union ppn ppn;
    ppn.raw = 0;

    ppn.bits.page = 1;
    taa_insert(&ctx, 0, ppn);      /* shard 0 */
    ppn.bits.page = 2;
    taa_insert(&ctx, 256, ppn);    /* shard 1 */
    ppn.bits.page = 3;
    taa_insert(&ctx, 512, ppn);    /* shard 2 */
    ppn.bits.page = 4;
    taa_insert(&ctx, 768, ppn);    /* shard 3 */

    TEST_ASSERT(taa_get_valid_count(&ctx) == 4, "4 entries across 4 shards");

    union ppn out;
    taa_lookup(&ctx, 0, &out);
    TEST_ASSERT(out.bits.page == 1, "shard 0 correct");
    taa_lookup(&ctx, 256, &out);
    TEST_ASSERT(out.bits.page == 2, "shard 1 correct");
    taa_lookup(&ctx, 512, &out);
    TEST_ASSERT(out.bits.page == 3, "shard 2 correct");
    taa_lookup(&ctx, 768, &out);
    TEST_ASSERT(out.bits.page == 4, "shard 3 correct");

    taa_cleanup(&ctx);
}

/* Concurrent access test — 4 threads each own a shard */
struct thread_arg {
    struct taa_ctx *ctx;
    u32 thread_id;
    u32 ops;
    int errors;
};

static void *concurrent_worker(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    struct taa_ctx *ctx = ta->ctx;
    u32 base = ta->thread_id * 256;
    ta->errors = 0;

    for (u32 i = 0; i < ta->ops; i++) {
        u64 lba = base + (i % 256);
        union ppn ppn;
        ppn.raw = 0;
        ppn.bits.channel = ta->thread_id;
        ppn.bits.page = i % 256;

        int ret = taa_insert(ctx, lba, ppn);
        if (ret != HFSSS_OK) { ta->errors++; continue; }

        union ppn out;
        ret = taa_lookup(ctx, lba, &out);
        if (ret != HFSSS_OK || out.raw != ppn.raw) { ta->errors++; }
    }
    return NULL;
}

static void test_concurrent(void)
{
    printf("\n=== TAA Concurrent Access (4 threads) ===\n");

    struct taa_ctx ctx;
    taa_init(&ctx, 1024, 2048, 4);

    pthread_t threads[4];
    struct thread_arg args[4];

    for (int i = 0; i < 4; i++) {
        args[i].ctx = &ctx;
        args[i].thread_id = i;
        args[i].ops = 10000;
        args[i].errors = 0;
        pthread_create(&threads[i], NULL, concurrent_worker, &args[i]);
    }

    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    int total_errors = 0;
    for (int i = 0; i < 4; i++) {
        total_errors += args[i].errors;
    }

    TEST_ASSERT(total_errors == 0, "0 errors across 40K concurrent ops");

    u64 lookups, conflicts;
    taa_get_stats(&ctx, &lookups, &conflicts);
    printf("  TAA stats: %" PRIu64 " lookups, %" PRIu64 " conflicts\n",
           lookups, conflicts);

    taa_cleanup(&ctx);
}

int main(void)
{
    printf("========================================\n");
    printf("TAA (Table Access Accelerator) Tests\n");
    printf("========================================\n");

    test_init_cleanup();
    test_lookup_insert_remove();
    test_update();
    test_cross_shard();
    test_concurrent();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Add Makefile target**

Add variable after `TEST_LARGE_CAP`:

```makefile
TEST_TAA = $(BIN_DIR)/test_taa
```

Add build rule:

```makefile
$(TEST_TAA): $(TEST_DIR)/test_taa.c $(LIBHFSSS_FTL) $(LIBHFSSS_COMMON)
	@echo "  CC      $@"
	@$(CC) $(CFLAGS) $< -o $@ -L$(LIB_DIR) -lhfsss-ftl -lhfsss-common -lm $(LDFLAGS)
```

Add `$(TEST_TAA)` to the `all:` target.

- [ ] **Step 3: Build and run**

```bash
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
./build/bin/test_taa
```

Expected: All tests pass, 0 errors across 40K concurrent ops.

- [ ] **Step 4: Commit**

```bash
git add tests/test_taa.c Makefile
git commit -m "test: add TAA unit tests (init, CRUD, cross-shard, 4-thread concurrent)"
```

---

### Task 3: Lock-Free I/O Queue

**Files:**
- Create: `include/ftl/io_queue.h`
- Create: `src/ftl/io_queue.c`
- Create: `tests/test_io_queue.c`
- Modify: `Makefile`

- [ ] **Step 1: Create I/O queue header**

Create `include/ftl/io_queue.h`:

```c
#ifndef __HFSSS_IO_QUEUE_H
#define __HFSSS_IO_QUEUE_H

#include "common/common.h"

/* I/O request opcode */
enum io_opcode {
    IO_OP_READ  = 0,
    IO_OP_WRITE = 1,
    IO_OP_TRIM  = 2,
    IO_OP_FLUSH = 3,
    IO_OP_STOP  = 4,   /* Sentinel: tells worker to exit */
};

/* I/O request — submitted from dispatch to FTL worker */
struct io_request {
    enum io_opcode opcode;
    u64            lba;
    u32            count;       /* Number of pages */
    u8            *data;        /* Pointer to caller buffer */
    u64            nbd_handle;  /* Echoed back in completion */
};

/* I/O completion — returned from FTL worker to dispatch */
struct io_completion {
    u64  nbd_handle;
    int  status;
};

/*
 * SPSC ring buffer (Single Producer Single Consumer) — lock-free.
 * Used for dispatch → worker queues (1 producer, 1 consumer).
 */
#define IO_RING_DEFAULT_CAPACITY 4096

struct io_ring {
    void             *slots;
    u32               slot_size;
    u32               capacity;      /* Must be power of 2 */
    volatile u32      head;          /* Written by consumer */
    volatile u32      tail;          /* Written by producer */
};

int  io_ring_init(struct io_ring *ring, u32 slot_size, u32 capacity);
void io_ring_cleanup(struct io_ring *ring);
bool io_ring_push(struct io_ring *ring, const void *item);
bool io_ring_pop(struct io_ring *ring, void *item);
bool io_ring_is_empty(struct io_ring *ring);
bool io_ring_is_full(struct io_ring *ring);
u32  io_ring_count(struct io_ring *ring);

#endif /* __HFSSS_IO_QUEUE_H */
```

- [ ] **Step 2: Implement I/O queue**

Create `src/ftl/io_queue.c`:

```c
#include "ftl/io_queue.h"
#include <stdlib.h>
#include <string.h>

static inline u32 next_power_of_2(u32 v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

int io_ring_init(struct io_ring *ring, u32 slot_size, u32 capacity)
{
    if (!ring || slot_size == 0 || capacity == 0) {
        return HFSSS_ERR_INVAL;
    }

    capacity = next_power_of_2(capacity);

    ring->slots = calloc(capacity, slot_size);
    if (!ring->slots) {
        return HFSSS_ERR_NOMEM;
    }

    ring->slot_size = slot_size;
    ring->capacity = capacity;
    ring->head = 0;
    ring->tail = 0;
    return HFSSS_OK;
}

void io_ring_cleanup(struct io_ring *ring)
{
    if (!ring) return;
    free(ring->slots);
    memset(ring, 0, sizeof(*ring));
}

bool io_ring_push(struct io_ring *ring, const void *item)
{
    u32 tail = ring->tail;
    u32 next = (tail + 1) & (ring->capacity - 1);

    /* Full? */
    if (next == __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE)) {
        return false;
    }

    memcpy((u8 *)ring->slots + tail * ring->slot_size,
           item, ring->slot_size);

    __atomic_store_n(&ring->tail, next, __ATOMIC_RELEASE);
    return true;
}

bool io_ring_pop(struct io_ring *ring, void *item)
{
    u32 head = ring->head;

    /* Empty? */
    if (head == __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE)) {
        return false;
    }

    memcpy(item, (u8 *)ring->slots + head * ring->slot_size,
           ring->slot_size);

    __atomic_store_n(&ring->head, (head + 1) & (ring->capacity - 1),
                     __ATOMIC_RELEASE);
    return true;
}

bool io_ring_is_empty(struct io_ring *ring)
{
    return __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
}

bool io_ring_is_full(struct io_ring *ring)
{
    u32 next = (__atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) + 1)
               & (ring->capacity - 1);
    return next == __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
}

u32 io_ring_count(struct io_ring *ring)
{
    u32 head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    u32 tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    return (tail - head) & (ring->capacity - 1);
}
```

- [ ] **Step 3: Write test**

Create `tests/test_io_queue.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ftl/io_queue.h"

static int total = 0, passed = 0, failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    total++; \
    if (cond) { printf("  [PASS] %s\n", msg); passed++; } \
    else      { printf("  [FAIL] %s\n", msg); failed++; } \
} while (0)

static void test_basic(void)
{
    printf("\n=== IO Ring Basic ===\n");

    struct io_ring ring;
    int ret = io_ring_init(&ring, sizeof(struct io_request),
                           IO_RING_DEFAULT_CAPACITY);
    TEST_ASSERT(ret == HFSSS_OK, "init succeeds");
    TEST_ASSERT(io_ring_is_empty(&ring), "initially empty");
    TEST_ASSERT(!io_ring_is_full(&ring), "initially not full");

    struct io_request req = { .opcode = IO_OP_WRITE, .lba = 42, .count = 1 };
    bool ok = io_ring_push(&ring, &req);
    TEST_ASSERT(ok, "push succeeds");
    TEST_ASSERT(!io_ring_is_empty(&ring), "not empty after push");
    TEST_ASSERT(io_ring_count(&ring) == 1, "count is 1");

    struct io_request out;
    ok = io_ring_pop(&ring, &out);
    TEST_ASSERT(ok, "pop succeeds");
    TEST_ASSERT(out.lba == 42, "popped LBA correct");
    TEST_ASSERT(out.opcode == IO_OP_WRITE, "popped opcode correct");
    TEST_ASSERT(io_ring_is_empty(&ring), "empty after pop");

    /* Pop from empty */
    ok = io_ring_pop(&ring, &out);
    TEST_ASSERT(!ok, "pop from empty returns false");

    io_ring_cleanup(&ring);
}

static void test_fill_drain(void)
{
    printf("\n=== IO Ring Fill/Drain ===\n");

    struct io_ring ring;
    io_ring_init(&ring, sizeof(u64), 16);

    /* Fill to capacity - 1 (one slot always empty in SPSC) */
    u32 pushed = 0;
    for (u32 i = 0; i < 100; i++) {
        u64 val = i;
        if (io_ring_push(&ring, &val)) pushed++;
    }
    TEST_ASSERT(pushed == 15, "pushed 15 items into 16-slot ring");
    TEST_ASSERT(io_ring_is_full(&ring), "ring is full");

    /* Drain all */
    u32 popped = 0;
    u64 val;
    while (io_ring_pop(&ring, &val)) popped++;
    TEST_ASSERT(popped == 15, "popped 15 items");
    TEST_ASSERT(io_ring_is_empty(&ring), "ring is empty");

    io_ring_cleanup(&ring);
}

/* SPSC concurrent: 1 producer thread, 1 consumer thread */
struct spsc_arg {
    struct io_ring *ring;
    u32 count;
    u64 checksum;
};

static void *producer_thread(void *arg)
{
    struct spsc_arg *a = (struct spsc_arg *)arg;
    for (u32 i = 0; i < a->count; i++) {
        u64 val = i;
        while (!io_ring_push(a->ring, &val)) {
            /* Spin until space available */
        }
        a->checksum += val;
    }
    return NULL;
}

static void *consumer_thread(void *arg)
{
    struct spsc_arg *a = (struct spsc_arg *)arg;
    for (u32 i = 0; i < a->count; i++) {
        u64 val;
        while (!io_ring_pop(a->ring, &val)) {
            /* Spin until data available */
        }
        a->checksum += val;
    }
    return NULL;
}

static void test_spsc_concurrent(void)
{
    printf("\n=== IO Ring SPSC Concurrent (100K ops) ===\n");

    struct io_ring ring;
    io_ring_init(&ring, sizeof(u64), 1024);

    struct spsc_arg prod = { .ring = &ring, .count = 100000, .checksum = 0 };
    struct spsc_arg cons = { .ring = &ring, .count = 100000, .checksum = 0 };

    pthread_t pt, ct;
    pthread_create(&pt, NULL, producer_thread, &prod);
    pthread_create(&ct, NULL, consumer_thread, &cons);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);

    TEST_ASSERT(prod.checksum == cons.checksum,
                "producer and consumer checksums match");
    TEST_ASSERT(io_ring_is_empty(&ring), "ring empty after test");

    printf("  Checksum: %llu\n", (unsigned long long)prod.checksum);

    io_ring_cleanup(&ring);
}

int main(void)
{
    printf("========================================\n");
    printf("IO Queue (Lock-Free Ring) Tests\n");
    printf("========================================\n");

    test_basic();
    test_fill_drain();
    test_spsc_concurrent();

    printf("\n========================================\n");
    printf("Results: %d/%d passed, %d failed\n", passed, total, failed);
    printf("========================================\n");
    return failed > 0 ? 1 : 0;
}
```

- [ ] **Step 4: Add Makefile targets, build and run**

Add `TEST_IO_QUEUE = $(BIN_DIR)/test_io_queue`, build rule, and add to `all:`.

```bash
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
./build/bin/test_io_queue
```

Expected: All tests pass, producer/consumer checksums match.

- [ ] **Step 5: Commit**

```bash
git add include/ftl/io_queue.h src/ftl/io_queue.c tests/test_io_queue.c Makefile
git commit -m "feat: add lock-free SPSC ring buffer for FTL I/O dispatch"
```

---

### Task 4: FTL Worker Threads

**Files:**
- Create: `include/ftl/ftl_worker.h`
- Create: `src/ftl/ftl_worker.c`

- [ ] **Step 1: Create FTL worker header**

Create `include/ftl/ftl_worker.h`:

```c
#ifndef __HFSSS_FTL_WORKER_H
#define __HFSSS_FTL_WORKER_H

#include "common/common.h"
#include "ftl/ftl.h"
#include "ftl/taa.h"
#include "ftl/io_queue.h"
#include <pthread.h>

#define FTL_NUM_WORKERS 4

/* FTL Worker Context — one per worker thread */
struct ftl_worker {
    u32             worker_id;
    pthread_t       thread;
    struct io_ring  request_ring;    /* SPSC: dispatch → worker */
    struct io_ring  completion_ring; /* SPSC: worker → dispatch */
    struct ftl_ctx *ftl;
    struct taa_ctx *taa;
    bool            running;
    u64             ops_completed;
    u64             ops_failed;
};

/* Multi-threaded FTL controller */
struct ftl_mt_ctx {
    struct ftl_ctx     ftl;
    struct taa_ctx     taa;
    struct ftl_worker  workers[FTL_NUM_WORKERS];
    bool               initialized;
};

/* Lifecycle */
int  ftl_mt_init(struct ftl_mt_ctx *ctx, struct ftl_config *config,
                 struct hal_ctx *hal);
void ftl_mt_cleanup(struct ftl_mt_ctx *ctx);

/* Start/stop worker threads */
int  ftl_mt_start(struct ftl_mt_ctx *ctx);
void ftl_mt_stop(struct ftl_mt_ctx *ctx);

/* Async I/O submission (from NBD dispatch thread) */
bool ftl_mt_submit(struct ftl_mt_ctx *ctx, const struct io_request *req);

/* Poll completions (from NBD dispatch thread) */
bool ftl_mt_poll_completion(struct ftl_mt_ctx *ctx,
                            struct io_completion *out);

#endif /* __HFSSS_FTL_WORKER_H */
```

- [ ] **Step 2: Implement FTL worker**

Create `src/ftl/ftl_worker.c`:

```c
#include "ftl/ftl_worker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *ftl_worker_main(void *arg)
{
    struct ftl_worker *w = (struct ftl_worker *)arg;
    struct io_request req;
    struct io_completion cpl;

    while (w->running) {
        if (!io_ring_pop(&w->request_ring, &req)) {
            /* No work — brief yield to avoid busy spin */
            sched_yield();
            continue;
        }

        if (req.opcode == IO_OP_STOP) {
            w->running = false;
            break;
        }

        int rc = HFSSS_OK;

        switch (req.opcode) {
        case IO_OP_READ: {
            /* Use TAA for L2P lookup, then read from NAND via FTL */
            u32 page_size = w->ftl->config.page_size;
            u8 *ptr = req.data;
            for (u32 i = 0; i < req.count; i++) {
                rc = ftl_read_page_mt(w->ftl, w->taa,
                                      req.lba + i, ptr);
                if (rc != HFSSS_OK) break;
                ptr += page_size;
            }
            break;
        }
        case IO_OP_WRITE: {
            u32 page_size = w->ftl->config.page_size;
            const u8 *ptr = req.data;
            for (u32 i = 0; i < req.count; i++) {
                rc = ftl_write_page_mt(w->ftl, w->taa,
                                       req.lba + i, ptr);
                if (rc != HFSSS_OK) break;
                ptr += page_size;
            }
            break;
        }
        case IO_OP_TRIM: {
            for (u32 i = 0; i < req.count; i++) {
                rc = ftl_trim_page_mt(w->ftl, w->taa, req.lba + i);
                if (rc != HFSSS_OK) break;
            }
            break;
        }
        case IO_OP_FLUSH:
            /* Flush is a no-op in DRAM-backed simulator */
            rc = HFSSS_OK;
            break;
        default:
            rc = HFSSS_ERR_INVAL;
            break;
        }

        cpl.nbd_handle = req.nbd_handle;
        cpl.status = rc;
        w->ops_completed++;
        if (rc != HFSSS_OK) w->ops_failed++;

        /* Push completion — spin if full (should not happen with
         * reasonable ring sizes) */
        while (!io_ring_push(&w->completion_ring, &cpl)) {
            sched_yield();
        }
    }

    return NULL;
}

int ftl_mt_init(struct ftl_mt_ctx *ctx, struct ftl_config *config,
                struct hal_ctx *hal)
{
    int ret;

    if (!ctx || !config || !hal) {
        return HFSSS_ERR_INVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Initialize base FTL */
    ret = ftl_init(&ctx->ftl, config, hal);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Initialize TAA with 256 shards */
    u64 total_pages = (u64)config->channel_count *
                      config->chips_per_channel *
                      config->dies_per_chip *
                      config->planes_per_die *
                      config->blocks_per_plane *
                      config->pages_per_block;

    ret = taa_init(&ctx->taa, config->total_lbas, total_pages,
                   TAA_DEFAULT_SHARDS);
    if (ret != HFSSS_OK) {
        ftl_cleanup(&ctx->ftl);
        return ret;
    }

    /* Initialize worker contexts (threads not started yet) */
    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        struct ftl_worker *w = &ctx->workers[i];
        w->worker_id = i;
        w->ftl = &ctx->ftl;
        w->taa = &ctx->taa;
        w->running = false;
        w->ops_completed = 0;
        w->ops_failed = 0;

        ret = io_ring_init(&w->request_ring, sizeof(struct io_request),
                           IO_RING_DEFAULT_CAPACITY);
        if (ret != HFSSS_OK) goto fail;

        ret = io_ring_init(&w->completion_ring, sizeof(struct io_completion),
                           IO_RING_DEFAULT_CAPACITY);
        if (ret != HFSSS_OK) goto fail;
    }

    ctx->initialized = true;
    return HFSSS_OK;

fail:
    ftl_mt_cleanup(ctx);
    return ret;
}

void ftl_mt_cleanup(struct ftl_mt_ctx *ctx)
{
    if (!ctx) return;

    ftl_mt_stop(ctx);

    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        io_ring_cleanup(&ctx->workers[i].request_ring);
        io_ring_cleanup(&ctx->workers[i].completion_ring);
    }

    taa_cleanup(&ctx->taa);
    ftl_cleanup(&ctx->ftl);
    memset(ctx, 0, sizeof(*ctx));
}

int ftl_mt_start(struct ftl_mt_ctx *ctx)
{
    if (!ctx || !ctx->initialized) {
        return HFSSS_ERR_INVAL;
    }

    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        ctx->workers[i].running = true;
        int ret = pthread_create(&ctx->workers[i].thread, NULL,
                                  ftl_worker_main, &ctx->workers[i]);
        if (ret != 0) {
            /* Stop already-started workers */
            for (int j = 0; j < i; j++) {
                struct io_request stop = { .opcode = IO_OP_STOP };
                io_ring_push(&ctx->workers[j].request_ring, &stop);
                pthread_join(ctx->workers[j].thread, NULL);
            }
            return HFSSS_ERR_INVAL;
        }
    }

    return HFSSS_OK;
}

void ftl_mt_stop(struct ftl_mt_ctx *ctx)
{
    if (!ctx) return;

    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        if (ctx->workers[i].running) {
            struct io_request stop = { .opcode = IO_OP_STOP };
            while (!io_ring_push(&ctx->workers[i].request_ring, &stop)) {
                sched_yield();
            }
            pthread_join(ctx->workers[i].thread, NULL);
            ctx->workers[i].running = false;
        }
    }
}

bool ftl_mt_submit(struct ftl_mt_ctx *ctx, const struct io_request *req)
{
    if (!ctx || !req) return false;

    /* Route by LBA to worker: worker_id = lba % NUM_WORKERS */
    u32 wid = (u32)(req->lba % FTL_NUM_WORKERS);
    return io_ring_push(&ctx->workers[wid].request_ring, req);
}

bool ftl_mt_poll_completion(struct ftl_mt_ctx *ctx,
                            struct io_completion *out)
{
    if (!ctx || !out) return false;

    /* Round-robin poll all workers for completions */
    for (int i = 0; i < FTL_NUM_WORKERS; i++) {
        if (io_ring_pop(&ctx->workers[i].completion_ring, out)) {
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 3: Add `ftl_read_page_mt` / `ftl_write_page_mt` / `ftl_trim_page_mt` to ftl.c**

These are TAA-backed versions of the existing `ftl_read_page` / `ftl_write_page` that use the TAA shard locks instead of the global FTL lock. Add to `src/ftl/ftl.c` and declare in `include/ftl/ftl.h`.

In `include/ftl/ftl.h`, add before `#endif`:

```c
/* Multi-threaded page operations — use TAA shards instead of global lock */
struct taa_ctx;  /* forward declaration */
int ftl_read_page_mt(struct ftl_ctx *ctx, struct taa_ctx *taa,
                     u64 lba, void *data);
int ftl_write_page_mt(struct ftl_ctx *ctx, struct taa_ctx *taa,
                      u64 lba, const void *data);
int ftl_trim_page_mt(struct ftl_ctx *ctx, struct taa_ctx *taa, u64 lba);
```

In `src/ftl/ftl.c`, add the implementations that mirror existing `ftl_read_page`/`ftl_write_page` but use `taa_lookup`/`taa_update` instead of `mapping_l2p`/`mapping_update`. The block_mgr lock is already separate from the mapping lock, so both can proceed concurrently.

- [ ] **Step 4: Build**

```bash
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -5
```

- [ ] **Step 5: Commit**

```bash
git add include/ftl/ftl_worker.h src/ftl/ftl_worker.c \
        include/ftl/ftl.h src/ftl/ftl.c Makefile
git commit -m "feat: add FTL worker threads with TAA-backed multi-threaded page I/O"
```

---

### Task 5: Background GC Thread

**Files:**
- Create: `src/ftl/gc_thread.c`
- Modify: `include/ftl/ftl_worker.h` (add GC thread fields)
- Modify: `src/ftl/ftl_worker.c` (start/stop GC thread)

- [ ] **Step 1: Add GC thread fields to `ftl_mt_ctx`**

In `include/ftl/ftl_worker.h`, add to `struct ftl_mt_ctx`:

```c
    /* Background GC thread */
    pthread_t        gc_thread;
    pthread_mutex_t  gc_mutex;
    pthread_cond_t   gc_cond;
    bool             gc_running;
```

- [ ] **Step 2: Implement GC thread**

Create `src/ftl/gc_thread.c`:

```c
#include "ftl/ftl_worker.h"
#include <stdio.h>
#include <string.h>

void *gc_thread_main(void *arg)
{
    struct ftl_mt_ctx *mt = (struct ftl_mt_ctx *)arg;
    struct ftl_ctx *ftl = &mt->ftl;

    while (mt->gc_running) {
        /* Wait for signal from FTL workers */
        pthread_mutex_lock(&mt->gc_mutex);
        while (mt->gc_running &&
               !gc_should_trigger(&ftl->gc, ftl->block_mgr.free_blocks)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;  /* Check every 1 second even without signal */
            pthread_cond_timedwait(&mt->gc_cond, &mt->gc_mutex, &ts);
        }
        pthread_mutex_unlock(&mt->gc_mutex);

        if (!mt->gc_running) break;

        /* Run GC cycle */
        int rc = gc_run(&ftl->gc, &ftl->block_mgr, &ftl->mapping,
                         ftl->hal);
        if (rc == HFSSS_OK) {
            ftl->stats.gc_count++;
        }
    }

    return NULL;
}
```

- [ ] **Step 3: Integrate GC thread start/stop into `ftl_mt_start`/`ftl_mt_stop`**

In `src/ftl/ftl_worker.c`, add GC thread start after worker thread creation in `ftl_mt_start()`:

```c
    /* Start GC background thread */
    pthread_mutex_init(&ctx->gc_mutex, NULL);
    pthread_cond_init(&ctx->gc_cond, NULL);
    ctx->gc_running = true;
    pthread_create(&ctx->gc_thread, NULL, gc_thread_main, ctx);
```

And stop in `ftl_mt_stop()`:

```c
    /* Stop GC thread */
    ctx->gc_running = false;
    pthread_cond_signal(&ctx->gc_cond);
    pthread_join(ctx->gc_thread, NULL);
    pthread_mutex_destroy(&ctx->gc_mutex);
    pthread_cond_destroy(&ctx->gc_cond);
```

- [ ] **Step 4: Build and run existing tests**

```bash
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
make test 2>&1 | grep -E "Results:|FAIL"
```

Expected: All existing tests still pass.

- [ ] **Step 5: Commit**

```bash
git add src/ftl/gc_thread.c include/ftl/ftl_worker.h src/ftl/ftl_worker.c Makefile
git commit -m "feat: add background GC thread with condition-variable wake"
```

---

### Task 6: Background WL/Read Disturb Thread

**Files:**
- Create: `src/ftl/wl_thread.c`
- Modify: `include/ftl/ftl_worker.h` (add WL thread fields)
- Modify: `src/ftl/ftl_worker.c` (start/stop WL thread)

- [ ] **Step 1: Add WL thread fields**

In `include/ftl/ftl_worker.h`, add to `struct ftl_mt_ctx`:

```c
    /* Background WL/Read Disturb thread */
    pthread_t  wl_thread;
    bool       wl_running;
```

- [ ] **Step 2: Implement WL thread**

Create `src/ftl/wl_thread.c`:

```c
#include "ftl/ftl_worker.h"
#include <stdio.h>
#include <unistd.h>

#define WL_INTERVAL_SEC  10
#define WL_THRESHOLD     100   /* Max erase count delta */
#define RD_THRESHOLD     100000 /* Read disturb threshold */

void *wl_thread_main(void *arg)
{
    struct ftl_mt_ctx *mt = (struct ftl_mt_ctx *)arg;
    struct ftl_ctx *ftl = &mt->ftl;

    while (mt->wl_running) {
        sleep(WL_INTERVAL_SEC);

        if (!mt->wl_running) break;

        /* Wear leveling: check erase count spread */
        wear_level_check(&ftl->wl, &ftl->block_mgr, &ftl->mapping,
                          ftl->hal, WL_THRESHOLD);
    }

    return NULL;
}
```

- [ ] **Step 3: Integrate into ftl_mt_start/stop**

- [ ] **Step 4: Build and test**

- [ ] **Step 5: Commit**

```bash
git add src/ftl/wl_thread.c include/ftl/ftl_worker.h src/ftl/ftl_worker.c Makefile
git commit -m "feat: add background wear-leveling/read-disturb thread"
```

---

### Task 7: Integrate Multi-Threaded FTL into NBD Server

**Files:**
- Modify: `src/vhost/hfsss_nbd_server.c`

- [ ] **Step 1: Replace synchronous sssim calls with ftl_mt_submit/poll**

In the NBD serve loop, replace the synchronous `nvme_uspace_read`/`nvme_uspace_write` calls with:

1. Parse NBD request → create `struct io_request`
2. `ftl_mt_submit()` → push to worker ring
3. After submitting, drain completions with `ftl_mt_poll_completion()`
4. For each completion, send NBD reply

The NBD dispatch thread becomes a non-blocking event loop: submit requests, poll completions, send replies.

- [ ] **Step 2: Handle FLUSH (broadcast to all workers)**

FLUSH must wait for all in-flight I/O to complete. Submit IO_OP_FLUSH to all 4 workers, wait for 4 completions, then reply.

- [ ] **Step 3: Build and run with `-s 512` to smoke test**

```bash
make -j$(sysctl -n hw.ncpu) 2>&1 | tail -3
./build/bin/hfsss-nbd-server -p 10809 -s 512 &
# Quick connectivity test
sleep 2 && kill %1
```

- [ ] **Step 4: Commit**

```bash
git add src/vhost/hfsss_nbd_server.c
git commit -m "feat: integrate multi-threaded FTL into NBD server (async dispatch)"
```

---

### Task 8: Multi-Threaded FTL Integration Test

**Files:**
- Create: `tests/test_mt_ftl.c`
- Modify: `Makefile`

- [ ] **Step 1: Write integration test**

Test that creates `ftl_mt_ctx`, starts workers, submits 10K random read/write I/Os from 4 producer threads, verifies data integrity.

- [ ] **Step 2: Add Makefile target, build and run**

- [ ] **Step 3: Commit**

```bash
git add tests/test_mt_ftl.c Makefile
git commit -m "test: add multi-threaded FTL integration test (4 threads, 10K ops, data verify)"
```

---

### Task 9: QEMU + fio Verification

- [ ] **Step 1: Start multi-threaded NBD server with `-s 16384`**
- [ ] **Step 2: Boot QEMU, verify NVMe device**
- [ ] **Step 3: Run fio verify suite**
- [ ] **Step 4: Compare throughput vs single-threaded baseline (target: ~4x)**

---

### Task 10: Final Verification + Regression

- [ ] **Step 1: `make clean && make` — zero errors**
- [ ] **Step 2: `make test` — all existing tests pass**
- [ ] **Step 3: Run test_taa, test_io_queue, test_mt_ftl**
- [ ] **Step 4: Run test_large_capacity**
- [ ] **Step 5: Commit summary**

```bash
git log --oneline -10
```
