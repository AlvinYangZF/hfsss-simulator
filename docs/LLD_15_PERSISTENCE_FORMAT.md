# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：持久化格式接口详细设计
**文档版本**：V1.1
**编制日期**：2026-04-22
**设计阶段**：V1.5 (Target)
**密级**：内部资料

## 修订历史

| 版本 | 日期       | 描述                                                         |
|------|------------|--------------------------------------------------------------|
| V1.0 | 2026-03-15 | 初稿（与实际代码存在偏差：文件命名、WAL 记录布局、头文件引用） |
| V1.1 | 2026-04-22 | 与现网代码对齐；更正头文件引用；Panic 转储移到后续版本         |

---

## 目录

1. [模块概述](#1-模块概述)
2. [功能需求详细分解](#2-功能需求详细分解)
3. [二进制格式详细设计](#3-二进制格式详细设计)
4. [头文件设计](#4-头文件设计)
5. [函数接口详细设计](#5-函数接口详细设计)
6. [流程图](#6-流程图)
7. [容量与性能分析](#7-容量与性能分析)
8. [测试要点](#8-测试要点)

---

## 1. 模块概述

持久化格式模块定义了 HFSSS 所有持久化状态的磁盘与 NAND 上的二进制布局。本文档为格式/协议规范，不涉及运行时逻辑，重点描述二进制字段排布、魔数、版本号及兼容性规则。

现网代码中，持久化状态由以下四种制品组成：

1. **Media 检查点文件**（`checkpoint.bin`）：NAND 页数据 + 每页元数据 + BBT 的完整或增量快照，由 Media 层生成。
2. **Superblock 内置检查点**：L2P 映射快照 + 操作日志，保存在预留 NAND 超级块内，由 FTL 生成。
3. **WAL（Write-Ahead Log）**：64 字节记录的内存环形缓冲，供崩溃恢复使用；可通过 `wal_save` / `wal_load` 可选地持久化到文件。
4. **NOR 镜像文件**：NOR Flash 的二进制镜像，由 LLD_14_NOR_FLASH 规范。

Panic 转储文件属于后续版本内容（见 §3.5）——当前启动流程仅通过标准日志接口输出 `[PANIC]` 级别记录。

所有多字节字段使用主机字节序（所有支持目标均为小端）。所有头部包含魔数，并在需要时包含 CRC 字段，读取前必须先校验。

**覆盖需求**：REQ-131（持久化格式接口规范），隐式依赖 REQ-051（持久化写入）、REQ-052（崩溃恢复）。

---

## 2. 功能需求详细分解

| 需求 ID | 需求描述                                                                         | 优先级 | 目标版本 |
|---------|----------------------------------------------------------------------------------|--------|----------|
| REQ-131 | 持久化格式接口规范：定义所有持久化制品的二进制布局、魔数、版本与兼容性规则       | P0     | V1.5     |
| REQ-051 | 持久化写入：Media 检查点 + Superblock 检查点 + WAL 的原子写入语义                | P0     | V1.5     |
| REQ-052 | 崩溃恢复：从最新有效检查点加载映射，按序重放 Journal / WAL 增量                   | P0     | V1.5     |

---

## 3. 二进制格式详细设计

### 3.1 Media 检查点文件（`checkpoint.bin`）

由 `media_save` / `media_save_incremental` 写入单一文件（参见 `src/media/media.c`）。文件路径由调用方提供，常用约定是 `<checkpoint_dir>/checkpoint.bin`。

**文件头部**（Media 层内部结构，不对外导出）：

```c
#define MEDIA_FILE_MAGIC   0x48465353u  /* 小端 "HFSS" */
#define MEDIA_FILE_VERSION 2u           /* 引入增量检查点支持 */

#define MEDIA_FILE_FLAG_FULL        0x00000000u
#define MEDIA_FILE_FLAG_INCREMENTAL 0x00000001u

struct media_file_header {
    u32                  magic;             /* MEDIA_FILE_MAGIC */
    u32                  version;           /* MEDIA_FILE_VERSION */
    u32                  flags;             /* FULL | INCREMENTAL */
    struct media_config  config;            /* Channel/Chip/Die/Plane/Block/Page 几何信息 */
    struct media_stats   stats;             /* 汇总计数器 */
    u64                  nand_data_offset;  /* NAND 数据区起始偏移 */
    u64                  bbt_offset;        /* BBT 区起始偏移 */
};
```

V1 向后兼容读取结构 `struct media_file_header_v1`（无 `flags` 字段）保留在代码中，使 V2 读取器可以打开 V1 文件，并以 V2 格式重新保存。新写入器不再产出 V1。

**NAND 数据区**：按 Channel → Chip → Die → Plane → Block → Page 顺序逐一写入。

块粒度：`dirty` (bool) + `state` (`enum block_state`, u32) + `pages_written` (u32)。若 `flags & INCREMENTAL` 且块未脏，仅写入单个 `dirty=false` 字节并跳过页级内容。

页粒度：`dirty` (bool) + `struct page_metadata` + `page_size` 字节数据 + `spare_size` 字节 OOB。

```c
struct page_metadata {
    enum page_state state;          /* erased / programmed / error */
    u64             program_ts;
    u32             erase_count;
    u32             bit_errors;
    u32             read_count;
};
```

**BBT 区**：紧随 NAND 数据区之后，由 `bbt_save()` 写入（参见 `src/media/bbt.c`）。

**增量模式注意事项。** `write_file_header` 始终按照*全量*布局计算 `bbt_offset`（块元数据 + 页元数据 + 页数据 + OOB），不受 `flags` 影响。当 `flags & INCREMENTAL` 时，干净块/页只占一个 `false` 字节，真实落盘数据比全量少，因此 `bbt_offset` 在增量文件中**并不**指向 BBT 实际起始位置 —— 该字段仅在 `FULL` 文件中可信。现网加载路径通过按配置几何顺序读取 NAND 数据区（区段可自描述大小），绕过 `bbt_offset` 字段。`tests/systest_persistence.c` 将此作为已知限制记录在案；若需要头部偏移在重载时权威可用，调用方应使用 `media_save` 而非 `media_save_incremental`。

### 3.2 Superblock / FTL L2P 检查点（NAND 内）

FTL 将 L2P 检查点和操作日志保存在预留的 NAND 超级块中，而不是另起一个宿主端文件。布局与魔数定义在 `include/ftl/superblock.h`。

```c
#define SB_HEADER_MAGIC 0x53425F48u  /* "SB_H" */
#define SB_CKPT_MAGIC   0x434B5054u  /* "CKPT" */
#define SB_JRNL_MAGIC   0x4A524E4Cu  /* "JRNL" */

enum sb_page_type {
    SB_PAGE_HEADER    = 0,
    SB_PAGE_CKPT_DATA = 1,
    SB_PAGE_JRNL_DATA = 2,
    SB_PAGE_CKPT_END  = 3,
};

/* 每个 superblock page 开头的 32 字节头部 */
struct sb_page_header {
    u32 magic;        /* SB_HEADER_MAGIC | SB_CKPT_MAGIC | SB_JRNL_MAGIC */
    u32 page_type;    /* enum sb_page_type */
    u64 sequence;     /* 单调序列号 */
    u32 page_index;   /* 当前逻辑段内的页偏移 */
    u32 total_pages;  /* 当前检查点/日志段的总页数 */
    u32 crc32;        /* 头部之后数据区的 CRC */
    u32 reserved;
};
```

**检查点负载**（`SB_PAGE_CKPT_DATA`）：以 `ckpt_entry` 数组形式流式写入：

```c
struct ckpt_entry {
    u64 lba;
    u64 ppn_raw;   /* 当 LBA 无映射时为 HFSSS_UNMAPPED */
};
```

**Journal 负载**（`SB_PAGE_JRNL_DATA`）：以 `journal_entry` 数组形式流式写入：

```c
enum journal_op { JRNL_OP_WRITE = 1, JRNL_OP_TRIM = 2 };

struct journal_entry {
    u32 op;
    u32 reserved;
    u64 lba;
    u64 ppn_raw;   /* WRITE 时为新 PPN；TRIM 时为 0 */
    u64 sequence;  /* journal 内单调 */
};
```

### 3.3 WAL（Write-Ahead Log）

布局与魔数定义在 `include/ftl/wal.h`。WAL 上下文以 64 字节记录的环形缓冲形式存在于内存中；`wal_save` / `wal_load` 提供可选的文件级持久化（用于干净断电重启）。

```c
#define WAL_RECORD_MAGIC  0x12345678u
#define WAL_COMMIT_MARKER 0xDEADBEEFu
#define WAL_RECORD_SIZE   64
#define WAL_MAX_RECORDS   (16 * 1024)

enum wal_rec_type {
    WAL_REC_L2P_UPDATE   = 1,
    WAL_REC_TRIM         = 2,
    WAL_REC_BBT_UPDATE   = 3,
    WAL_REC_SMART_UPDATE = 4,
    WAL_REC_COMMIT       = 0xFF,
};

/* 64 字节记录 */
struct wal_record {
    u32 magic;       /* WAL_RECORD_MAGIC */
    u32 type;        /* enum wal_rec_type */
    u64 sequence;    /* WAL 内单调 */
    u64 lba;
    u64 ppn;
    u8  meta[16];    /* 类型相关元数据 */
    u32 crc32;       /* 字节 [0..55] 上的 CRC-32 */
    u32 end_marker;  /* 数据记录为 0，COMMIT 为 WAL_COMMIT_MARKER */
};
```

`WAL_REC_COMMIT` 记录携带 `end_marker = WAL_COMMIT_MARKER`，并推进 `wal_get_committed_seq` 维护的"最新已持久化提交序号"计数器。该计数器是独立辅助接口，供调用方查询最后一个安全点；**重放路径本身不使用它**。

`wal_replay` 按顺序遍历内存环形缓冲，逐条校验 `magic` + `crc32`，遇到首个无效记录时停止；遇到 `WAL_REC_COMMIT` 时跳过（控制标记，不向上转发），其余有效记录全部通过回调向上转发。当前实现**没有**"丢弃最后一条 COMMIT 之后记录"的行为 —— 首个损坏位之前的每一条有效记录都会被重放。

磁盘持久化时，WAL 文件是 `struct wal_record` 的扁平追加，**没有任何文件前导**：`wal_save` 直接写入 `count × sizeof(struct wal_record)` 字节；`wal_load` 逐条读取，遇到魔数不匹配、CRC 错误或达到环形缓冲容量上限时停止。

### 3.4 NOR 镜像

NOR 分区表及 `mmap(MAP_SHARED)` 磁盘布局（bootloader / fw_slot_a / fw_slot_b / config / bbt / event_log / sysinfo / keys）由 LLD_14_NOR_FLASH 完整说明，此处不再复述。

### 3.5 Panic 转储（后续版本）

V1.0 初稿曾规定过 `panic_dump_<timestamp>.bin` 文件格式（400 字节头部）。该设计未在当前版本落地：`src/common/log.c` 中的 `hfsss_panic()` 直接向 `stderr` 打印 `HFSSS PANIC`（绕过结构化日志通道），当前启动流程依靠 Superblock 检查点 + WAL 来完成事后状态恢复。独立的 panic-dump 文件格式保留为后续版本工作项；V1.0 的设计文稿保留在历史版本中，作为该工作的起点。

### 3.6 版本兼容性规则

- `current_version < min_reader_version`：拒绝加载，要求显式迁移；
- `writer_version > current_version`：允许加载并发出警告（向前兼容读取，尾部新增字段忽略）；
- 魔数不匹配：立即拒绝；
- CRC 不匹配：拒绝该制品并向上返回 I/O 错误。现网实现**不会**回退到更早的代数 —— `sb_checkpoint_read` 只选取单个最高序号的 superblock 头，若其数据页 CRC 校验失败即返回 `HFSSS_ERR_IO`；Media 检查点是单文件，CRC 错误也无上一代可退。

对于 Media 检查点，V1 → V2 升级通过 `struct media_file_header_v1` 自动完成；新写入器不再产出 V1。

---

## 4. 头文件设计

持久化类型分布在三处实际头文件中，**没有统一的** `include/common/persistence_fmt.h`。

| 头文件                         | 类型                                                                                                        |
|--------------------------------|-------------------------------------------------------------------------------------------------------------|
| `include/ftl/superblock.h`     | `SB_*_MAGIC`、`enum sb_page_type`、`struct sb_page_header`、`struct ckpt_entry`、`struct journal_entry`、`enum journal_op` |
| `include/ftl/wal.h`            | `WAL_RECORD_MAGIC`、`WAL_COMMIT_MARKER`、`enum wal_rec_type`、`struct wal_record`、`struct wal_ctx`           |
| `src/media/media.c`（静态）    | `MEDIA_FILE_MAGIC`、`MEDIA_FILE_VERSION`、`struct media_file_header`、`struct media_file_header_v1`、`struct page_metadata` |

Media 层的类型目前刻意保留为内部实现细节，因为检查点文件仅由 `media_save` / `media_load` 消费。若将来需要外部工具（例如离线分析器）消费该文件，相关 struct 将移入公共头文件。

---

## 5. 函数接口详细设计

### 5.1 Media 检查点

```c
int media_save(struct media_ctx *ctx, const char *filepath);             /* 全量 */
int media_save_incremental(struct media_ctx *ctx, const char *filepath); /* 增量 */
int media_load(struct media_ctx *ctx, const char *filepath);
```

- `media_save` 写入 `flags = MEDIA_FILE_FLAG_FULL` 的 V2 头部，随后写全量 NAND 数据和 BBT；
- `media_save_incremental` 写入 `flags = MEDIA_FILE_FLAG_INCREMENTAL` 的同版本头部，干净块/页仅写一个 bool 并跳过内容；
- `media_load` 读取头部，若是 V1 通过兼容 shim 识别并升级，再把 NAND 数据和 BBT 还原到内存 Media 上下文。

### 5.2 Superblock / L2P 检查点

```c
int  sb_checkpoint_write(struct superblock_ctx *sb, struct mapping_ctx *mapping);
int  sb_checkpoint_read (struct superblock_ctx *sb, struct mapping_ctx *mapping);
int  sb_journal_append  (struct superblock_ctx *sb, enum journal_op op, u64 lba, u64 ppn_raw);
int  sb_journal_flush   (struct superblock_ctx *sb);
int  sb_journal_replay  (struct superblock_ctx *sb, struct mapping_ctx *mapping);
int  sb_recover         (struct superblock_ctx *sb, struct mapping_ctx *mapping, struct block_mgr *mgr);
bool sb_has_valid_checkpoint(struct superblock_ctx *sb);
```

检查点页以 `SB_HEADER_MAGIC` 起始页为头，若干 `SB_CKPT_MAGIC` 数据页为体，由 `SB_PAGE_CKPT_END` 结束。每一页都带有独立的 `sb_page_header` + `crc32`。Journal 通过 `sb_journal_append` 追加，`sb_journal_flush` 强制活动页下刷。恢复流程先应用最新有效检查点，再按序重放 Journal。

### 5.3 WAL

```c
int  wal_init    (struct wal_ctx *ctx, u32 max_records);
void wal_cleanup (struct wal_ctx *ctx);

int  wal_append  (struct wal_ctx *ctx, enum wal_rec_type type,
                  u64 lba, u64 ppn, const void *meta, u32 meta_len);
int  wal_commit  (struct wal_ctx *ctx);
int  wal_replay  (const struct wal_ctx *ctx, wal_replay_cb cb, void *user_data);

u64  wal_get_committed_seq(const struct wal_ctx *ctx);
int  wal_truncate(struct wal_ctx *ctx, u64 up_to_seq);
u32  wal_get_count (const struct wal_ctx *ctx);
void wal_reset     (struct wal_ctx *ctx);

int  wal_save    (const struct wal_ctx *ctx, const char *filepath);
int  wal_load    (struct wal_ctx *ctx, const char *filepath);
```

`wal_append` 为新记录打上 `WAL_RECORD_MAGIC`、单调序号、类型、LBA、PPN、类型相关元数据，并计算字节 [0..55] 上的 CRC-32。`wal_commit` 追加一条 `end_marker = WAL_COMMIT_MARKER` 的 `WAL_REC_COMMIT` 记录。

`wal_replay` 按顺序校验每条记录的 magic + CRC，遇到首个无效槽即停止；跳过 COMMIT 记录（控制标记，不向上转发），其余有效记录全部通过回调向上转发。**不会**查询 `wal_get_committed_seq`，因此不存在"丢弃最后一条 COMMIT 之后记录"的行为 —— 首个损坏槽之前的每条有效记录都会被重放。`wal_get_committed_seq` 作为独立辅助接口保留，供需要最后一个持久化提交点的调用方单独使用。

注意：`wal_replay` 目前是已导出的 API，但**没有生产调用方** —— 现网恢复路径完全由 superblock 驱动（参见下文 `sb_recover`）。WAL 覆盖仅在单元测试中实现（`tests/test_foundation.c`）。

---

## 6. 流程图

### 6.1 全量检查点写流程

```
media_save(ctx, "checkpoint.bin")
  -> fopen(filepath, "wb")
  -> write_file_header(flags = FULL)
  -> write_nand_data(full)          # 所有 block + page + metadata
  -> bbt_save()                     # BBT 区
  -> fclose()
```

### 6.2 增量检查点写流程

```
media_save_incremental(ctx, "checkpoint.bin")
  -> fopen(filepath, "wb")
  -> write_file_header(flags = INCREMENTAL)
  -> write_nand_data(incremental)   # 干净 block/page 仅写一个 false 字节
  -> bbt_save()
  -> fclose()
```

### 6.3 崩溃恢复流程（FTL 侧）

```
启动
  -> sb_recover(sb, mapping, mgr)
       -> sb_has_valid_checkpoint?
            否 -> 冷启动
            是 -> sb_checkpoint_read
                  -> sb_journal_replay (按序应用 WRITE / TRIM 增量)
  -> 恢复完成
```

`wal_replay` 未接入生产恢复路径（`sssim_init` / `ftl_init` 只调用 `sb_recover`）。该 API 作为单独工具保留，便于调用方在内存环形缓冲之上自行扩展重放；`tests/test_foundation.c` 在单元层面覆盖它。

### 6.4 Superblock 页完整性检查

```
读取页开头的 sb_page_header
  magic 属于 { SB_HEADER_MAGIC, SB_CKPT_MAGIC, SB_JRNL_MAGIC } ?
    否 -> 拒绝该页，返回 HFSSS_ERR_IO（无上一代回退）
    是 -> 重新计算数据区 CRC
           与 header.crc32 匹配 ?
             否 -> 拒绝该页，返回 HFSSS_ERR_IO
             是 -> 按 page_type 应用
```

`sb_checkpoint_read` 扫描每个预留 superblock 的第 0 页，选取头部 `sequence` 最高的 block，并从该 block 读取检查点其余部分。若所选 block 的任意数据页 CRC 校验失败，读取直接终止并返回 `HFSSS_ERR_IO` —— 现网代码**不会**回退至更早代数的 superblock。

### 6.5 WAL 记录完整性检查

```
对 WAL 环中每个 64 字节槽：
  magic == WAL_RECORD_MAGIC ?
    否 -> 停止重放（空/损坏尾部）
    是 -> 重新计算字节 [0..55] 上的 CRC
           与 record.crc32 匹配 ?
             否 -> 停止重放（记录损坏）
             是 -> 若 type == WAL_REC_COMMIT
                      -> 跳过（控制标记，不向上转发回调）
                    否则
                      -> 转发 callback(rec, user_data)
```

`wal_get_committed_seq` 独立遍历环形缓冲查找最高序号的 COMMIT，不属于重放流程。需要关心最后一个持久化提交点的生产代码应单独调用该辅助接口。

---

## 7. 容量与性能分析

下表基于 2 TB 几何（512 M LPN，4 KiB 页）：

| 指标                                              | 计算方式                                 | 结果                      |
|---------------------------------------------------|------------------------------------------|---------------------------|
| Media 检查点（全量，2 TB NAND 数据）              | `total_pages × (page_size + spare_size)` | ≈ 数据体量 + 元数据（~1%）|
| Media 检查点（增量，空闲态）                      | 每块一个 bool + BBT                       | ~ total_blocks 字节 + BBT |
| Superblock L2P 检查点大小                         | `512 M × 16 B (ckpt_entry)`              | 分摊到超级块页上共 8 GB   |
| WAL 内存环形缓冲                                  | `WAL_MAX_RECORDS × 64 B`                 | 1 MB                      |
| WAL 磁盘文件（启用持久化时）                      | `count × 64 B`（无前导）                  | ≤ 1 MB                    |
| Superblock 页头开销                               | `sizeof(struct sb_page_header)`           | 32 B / 页                 |
| `wal_append` 延迟（仅内存）                        | memcpy + CRC                             | < 1 µs                    |
| `wal_save` 延迟（落盘）                            | 单次 `fwrite` + `fclose`                  | ~100 µs                   |

---

## 8. 测试要点

现网持久化测试分布：

| 测试 ID     | 源文件                                           | 覆盖                                           |
|-------------|--------------------------------------------------|------------------------------------------------|
| PF-SB-*     | `tests/test_superblock.c`                        | Superblock 头/检查点/Journal 往返、魔数与 CRC 拒绝、恢复 |
| PF-PWR-*    | `tests/test_power_cycle.c`                       | 端到端崩溃恢复（`media_save` + 重启 + `media_load` + Journal 重放） |
| PF-MEDIA-*  | `tests/test_media.c::test_persistence`           | Media 检查点在 `media_save` / `media_load` 层的读写（仅全量） |
| PF-PER-*    | `tests/systest_persistence.c`                    | 全量与增量检查点行为，包括 §3.1 记录的 `bbt_offset` 限制 |
| PF-WAL-*    | `tests/test_foundation.c`（WAL 单元章节）        | `wal_init` / `wal_append` / `wal_commit` / `wal_replay` / `wal_save` / `wal_load` 往返、魔数与 CRC 拒绝 |

代表性测试点：

| 用例 ID | 描述                                                                 | 验证点                                                         |
|---------|----------------------------------------------------------------------|----------------------------------------------------------------|
| PF-001  | Superblock 检查点写然后读                                            | 所有 LBA → PPN 映射往返一致                                    |
| PF-002  | 损坏 `sb_page_header.crc32`                                           | `sb_checkpoint_read` 返回 `HFSSS_ERR_IO`；无上一代重试          |
| PF-003  | 损坏检查点 entry 区                                                  | CRC 不匹配 → 拒绝并返回 `HFSSS_ERR_IO`                          |
| PF-004  | WAL 追加 N 条记录，`wal_replay` 带回调                                | 每条有效非 COMMIT 记录转发到回调；COMMIT 记录被跳过            |
| PF-005  | WAL 最终槽（`magic = 0`）截断                                         | 重放在该槽停止；之前的记录仍被转发                             |
| PF-006  | WAL 记录 CRC 错误                                                    | 在第 N-1 条停止；之后无记录转发                                |
| PF-007  | 任一持久化制品的魔数不匹配                                           | 读取器拒绝，不尝试解析负载                                     |
| PF-008  | Media 检查点：保存（全量）→ 重新打开 → 加载 → 校验页数据 + OOB        | 页数据 + 元数据一致                                            |
| PF-009  | Media 检查点：部分脏后保存（增量）                                   | 干净块被跳过；脏块往返一致；`bbt_offset` 偏差由顺序加载容忍   |
| PF-010  | `MEDIA_FILE_MAGIC` 不匹配                                             | `media_load` 返回 `HFSSS_ERR_INVAL`                             |
| PF-011  | Media V1 → V2 升级路径                                                | V1 文件通过 `_v1` shim 正常加载                                 |
| PF-012  | 冷启动（无检查点、无 Journal）                                       | FTL 以空映射启动，无错误                                       |
| PF-013  | 热启动：有效检查点 + 空 Journal                                      | 映射与关机前一致                                               |
| PF-014  | 热启动：有效检查点 + 非空 Journal                                    | 先应用检查点，再按序重放 Journal 增量                          |
| PF-015  | 完整崩溃恢复流程（superblock 检查点 + journal；`wal_replay` 仅单元测试独立覆盖） | 末态与最后一次持久化提交的写入保持一致 |

---

**文档统计**：
- 覆盖需求：REQ-131（主），REQ-051、REQ-052（隐式）
- 头文件：`include/ftl/superblock.h`、`include/ftl/wal.h`、Media 内部（`src/media/media.c`）
- 函数接口：Media 3 个、Superblock 7 个、WAL 11 个
- 测试用例：15 个（15 个已由现网测试覆盖）

## 附录：交叉引用

| 引用内容                    | 所属文档                       |
|-----------------------------|--------------------------------|
| Bootloader 恢复路径         | LLD_09_BOOTLOADER              |
| L2P 检查点触发              | LLD_11_FTL_RELIABILITY         |
| NOR Flash 镜像格式          | LLD_14_NOR_FLASH               |
| 掉电与 WAL 协同             | LLD_17_POWER_LOSS_PROTECTION   |
