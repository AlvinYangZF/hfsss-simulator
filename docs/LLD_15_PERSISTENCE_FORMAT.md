# 高保真全栈SSD模拟器（HFSSS）详细设计文档

**文档名称**：持久化格式接口详细设计
**文档版本**：V1.0
**编制日期**：2026-03-15
**设计阶段**：V1.5 (Target)
**密级**：内部资料

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

持久化格式模块（Persistence Format）定义了HFSSS全部持久化状态的磁盘二进制布局。本文档为纯格式/协议规范，不涉及运行时逻辑，重点描述二进制字段排布、魔数、版本号及兼容性规则。

持久化状态由以下五类文件组成：

1. **NAND数据文件**：存储原始NAND页数据，每个Plane对应一个独立文件；
2. **L2P检查点文件**：存储逻辑地址到物理地址映射表（L2P Table）的完整快照；
3. **WAL文件（预写日志）**：存储增量映射更新，用于崩溃恢复；
4. **NOR镜像文件**：NOR Flash二进制镜像（由LLD_14覆盖，本文档仅作引用）；
5. **Panic转储文件**：崩溃现场状态转储，用于事后调试分析。

所有文件均采用小端字节序（little-endian）。所有头部区域均包含CRC校验字段，读取时必须首先验证校验值。版本字段遵循向前兼容规则（见3.5节）。

**覆盖需求**：REQ-131（持久化格式接口规范），隐式依赖 REQ-051（持久化写入）、REQ-052（崩溃恢复）。

---

## 2. 功能需求详细分解

| 需求ID | 需求描述 | 优先级 | 目标版本 |
|--------|----------|--------|----------|
| REQ-131 | 持久化格式接口规范：定义所有磁盘文件的二进制布局、魔数、版本与兼容性规则 | P0 | V1.5 |
| REQ-051 | 持久化写入：L2P检查点与WAL文件的原子写入语义 | P0 | V1.5 |
| REQ-052 | 崩溃恢复：启动时从最新有效检查点加载L2P，并重放WAL增量记录 | P0 | V1.5 |

---

## 3. 二进制格式详细设计

### 3.1 NAND数据文件格式

每个Plane对应一个文件，命名规则：

```
<checkpoint_dir>/nand_ch<CH>_chip<CHIP>_die<DIE>_plane<PLANE>.dat
```

文件总体布局：

```
偏移   大小  字段
0      8     魔数: "HFSSS_ND" (0x4e44_5353_4648 + 版本字节)
8      4     格式版本: uint32_t（当前值: 1）
12     4     标志位: uint32_t（bit0=已压缩, bit1=已加密）
16     4     Channel ID: uint32_t
20     4     Chip ID: uint32_t
24     4     Die ID: uint32_t
28     4     Plane ID: uint32_t
32     4     每Plane块数: uint32_t
36     4     每块页数: uint32_t
40     4     页数据大小（字节）: uint32_t
44     4     OOB大小（字节）: uint32_t
48     8     文件创建时间戳: uint64_t（纳秒，自epoch起）
56     8     最后修改时间戳: uint64_t
64     8     文件总大小（字节）: uint64_t
72     4     头部CRC32（覆盖字节0-71）: uint32_t
76     4     保留: uint32_t（必须为0）
80     =     数据区: blocks × pages × (page_size + oob_size) 字节
             布局: block0_page0_data[page_size] + block0_page0_oob[oob_size]
                   block0_page1_data + block0_page1_oob ...
             未写入页填充0xFF
```

OOB区布局（每页，oob_size = 384字节）：

```
偏移   大小  字段
0      8     LPN（逻辑页号）: uint64_t（0xFFFFFFFFFFFFFFFF = 未写入）
8      8     写入时间戳: uint64_t
16     4     ECC校正字节长度: uint32_t
20     344   ECC校正字节: uint8_t[344]
364    4     OOB CRC32（覆盖字节0-363）: uint32_t
368    16    保留: uint8_t[16]
```

### 3.2 L2P检查点文件格式

每次检查点对应一个文件，命名规则：

```
<checkpoint_dir>/l2p_checkpoint_<SEQ>.dat
```

文件布局：

```
偏移   大小  字段
0      8     魔数: "HFSSS_L2" (0x324c5f53535346 48)
8      4     格式版本: uint32_t（当前值: 1）
12     4     标志位: uint32_t
16     8     LPN总数: uint64_t（例如 2TB/4KB = 536,870,912）
24     8     检查点序列号: uint64_t（单调递增）
32     8     检查点时间戳: uint64_t（纳秒，自epoch起）
40     8     检查点时刻的主机累计写入字节数: uint64_t
48     4     数据区CRC64低32位: uint32_t
52     4     数据区CRC64高32位: uint32_t
56     4     头部CRC32（覆盖字节0-55）: uint32_t
60     4     保留: uint32_t
64     =     数据区: uint64_t[LPN总数]
             索引 = LPN，值 = PPN（0xFFFFFFFFFFFFFFFF = 未映射）
             PPN编码: bits[63:48]=channel, bits[47:32]=chip, bits[31:16]=die,
                      bits[15:8]=plane, bits[7:0]=block_msb（完整PPN见LLD_06）
```

检查点文件命名约定：保留最新3代检查点文件；写入新检查点后删除最旧的文件。

### 3.3 WAL文件格式

每个WAL目录下一个文件：

```
<wal_dir>/hfsss.wal
```

WAL文件采用顺序追加写入；成功完成检查点后截断至仅保留文件头。

**WAL文件头（固定64字节）：**

```
偏移   大小  字段
0      8     魔数: "HFSSS_WL" (0x4c575f53535346 48)
8      4     格式版本: uint32_t（当前值: 1）
12     4     标志位: uint32_t
16     8     关联检查点序列号: uint64_t
24     8     文件创建时间戳: uint64_t
32     4     记录数量: uint32_t（正常关闭时更新）
36     4     头部CRC32: uint32_t
40     24    保留
```

**WAL记录（固定64字节，必须严格为64字节）：**

```
偏移   大小  字段
0      8     序列号: uint64_t（从1开始单调递增）
8      4     记录类型: uint32_t（见下方枚举）
12     4     载荷长度: uint32_t（≤ 40字节）
16     40    载荷: uint8_t[40]
56     4     记录CRC32（覆盖字节0-55）: uint32_t
60     4     保留: uint32_t（必须为0xDEADBEEF，作为结束标记）
```

WAL记录类型及载荷布局：

```c
enum wal_record_type {
    WAL_REC_L2P_UPDATE       = 0x0001, /* LPN→PPN映射变更 */
    WAL_REC_BLOCK_STATE      = 0x0002, /* 块状态转换 */
    WAL_REC_ERASE_COUNT      = 0x0003, /* 块擦除计数递增 */
    WAL_REC_BAD_BLOCK        = 0x0004, /* 新坏块检测 */
    WAL_REC_CHECKPOINT_BEGIN = 0x0010, /* 检查点开始（用于恢复检测） */
    WAL_REC_CHECKPOINT_END   = 0x0011, /* 检查点结束（WAL可截断的信号） */
    WAL_REC_SHUTDOWN_CLEAN   = 0x0020, /* 正常关机标记 */
    WAL_REC_SHUTDOWN_PANIC   = 0x0021, /* Panic关机标记 */
};

/* WAL_REC_L2P_UPDATE载荷（40字节）: */
struct wal_payload_l2p {
    uint64_t lpn;        /* 逻辑页号 */
    uint64_t old_ppn;    /* 原物理页（0xFFFF...表示原先未映射） */
    uint64_t new_ppn;    /* 新物理页 */
    uint64_t timestamp;  /* 纳秒，自epoch起 */
    uint8_t  reserved[8];
};  /* 共40字节 */

/* WAL_REC_BLOCK_STATE载荷: */
struct wal_payload_block_state {
    uint8_t  channel, chip, die, plane;
    uint32_t block;
    uint8_t  old_state;    /* enum block_state取值 */
    uint8_t  new_state;
    uint8_t  reserved[30];
};  /* 共40字节 */

/* WAL_REC_ERASE_COUNT载荷: */
struct wal_payload_erase_count {
    uint8_t  channel, chip, die, plane;
    uint32_t block;
    uint32_t erase_count;
    uint8_t  reserved[28];
};  /* 共40字节 */
```

### 3.4 Panic转储文件格式

文件路径：`/var/hfsss/panic_dump_<timestamp>.bin`

```
偏移   大小  字段
0      8     魔数: "HFSSS_PD"
8      4     格式版本: uint32_t
12     4     固件版本（构建哈希）: uint32_t
16     8     Panic时间戳: uint64_t
24     4     Panic原因码: uint32_t
28     4     Panic线程ID: uint32_t
32     256   Panic消息（null终止字符串）: char[256]
288    64    调用栈帧（前8帧 × 8字节）: uint64_t[8]
352    8     Panic前最后一次L2P检查点序列号: uint64_t
360    8     Panic前最后一条WAL序列号: uint64_t
368    ...   部分L2P表摘要（前1000条与后1000条映射项）
```

### 3.5 版本兼容性规则

所有文件头均嵌入如下兼容性字段：

```c
#define HFSSS_FORMAT_MAGIC_ND  0x444E5F535353484658ULL  /* "HFSSSNF_ND" */
#define HFSSS_FORMAT_MAGIC_L2  0x324C5F535353464855ULL  /* "HFSSSL2_"  */
#define HFSSS_FORMAT_MAGIC_WAL 0x4C575F535353464856ULL  /* "HFSSWL_"   */

struct format_version_compat {
    uint32_t min_reader_version;  /* 能读取此文件的最低软件版本 */
    uint32_t writer_version;      /* 写入此文件的软件版本 */
};
```

兼容性判断规则：

- 若 `current_version < min_reader_version`：拒绝打开，提示需要迁移；
- 若 `writer_version > current_version`：打开并发出警告（前向兼容），不崩溃；
- 魔数不匹配：立即拒绝，返回错误；
- 头部CRC不匹配：立即拒绝，不尝试读取数据区。

---

## 4. 头文件设计

```c
/* include/common/persistence_fmt.h */
#ifndef HFSSS_PERSISTENCE_FMT_H
#define HFSSS_PERSISTENCE_FMT_H

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

/* ------------------------------------------------------------------ */
/* Magic numbers                                                        */
/* ------------------------------------------------------------------ */

#define HFSSS_FMT_MAGIC_ND   UINT64_C(0x444E5F535353484658)  /* NAND data file   */
#define HFSSS_FMT_MAGIC_L2   UINT64_C(0x324C5F535353464855)  /* L2P checkpoint   */
#define HFSSS_FMT_MAGIC_WAL  UINT64_C(0x4C575F535353464856)  /* WAL file         */
#define HFSSS_FMT_MAGIC_PD   UINT64_C(0x44505F535353464857)  /* Panic dump       */

#define HFSSS_FMT_VERSION_CURRENT  1u

/* WAL record end marker – occupies the last 4 bytes of every WAL record */
#define HFSSS_WAL_END_MARKER  0xDEADBEEFu

/* Sentinel value for unmapped LPN/PPN entries */
#define HFSSS_UNMAPPED        UINT64_C(0xFFFFFFFFFFFFFFFF)

/* ------------------------------------------------------------------ */
/* Version compatibility descriptor                                     */
/* ------------------------------------------------------------------ */

struct hfsss_fmt_compat {
    uint32_t min_reader_version;
    uint32_t writer_version;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* NAND data file header (80 bytes)                                     */
/* ------------------------------------------------------------------ */

struct nand_file_header {
    uint64_t magic;               /* HFSSS_FMT_MAGIC_ND                        */
    uint32_t format_version;      /* HFSSS_FMT_VERSION_CURRENT                 */
    uint32_t flags;               /* bit0=compressed, bit1=encrypted           */
    uint32_t channel_id;
    uint32_t chip_id;
    uint32_t die_id;
    uint32_t plane_id;
    uint32_t blocks_per_plane;
    uint32_t pages_per_block;
    uint32_t page_size_bytes;
    uint32_t oob_size_bytes;
    uint64_t create_timestamp_ns;
    uint64_t modify_timestamp_ns;
    uint64_t total_file_size;
    uint32_t header_crc32;        /* CRC32 of bytes 0–71                       */
    uint32_t reserved;            /* must be 0                                 */
} __attribute__((packed));

static_assert(sizeof(struct nand_file_header) == 80,
              "nand_file_header must be exactly 80 bytes");

/* ------------------------------------------------------------------ */
/* NAND OOB region per page (384 bytes)                                 */
/* ------------------------------------------------------------------ */

struct nand_oob_entry {
    uint64_t lpn;                 /* HFSSS_UNMAPPED if page not written        */
    uint64_t write_timestamp_ns;
    uint32_t ecc_syndrome_len;
    uint8_t  ecc_syndrome[344];
    uint32_t oob_crc32;           /* CRC32 of bytes 0–363                      */
    uint8_t  reserved[16];
} __attribute__((packed));

static_assert(sizeof(struct nand_oob_entry) == 384,
              "nand_oob_entry must be exactly 384 bytes");

/* ------------------------------------------------------------------ */
/* L2P checkpoint file header (64 bytes)                                */
/* ------------------------------------------------------------------ */

struct l2p_checkpoint_header {
    uint64_t magic;               /* HFSSS_FMT_MAGIC_L2                        */
    uint32_t format_version;
    uint32_t flags;
    uint64_t total_lpn_count;
    uint64_t checkpoint_seq;      /* monotonically increasing                  */
    uint64_t checkpoint_timestamp_ns;
    uint64_t host_bytes_written;
    uint32_t data_crc64_lo;
    uint32_t data_crc64_hi;
    uint32_t header_crc32;        /* CRC32 of bytes 0–55                       */
    uint32_t reserved;
} __attribute__((packed));

static_assert(sizeof(struct l2p_checkpoint_header) == 64,
              "l2p_checkpoint_header must be exactly 64 bytes");

/* ------------------------------------------------------------------ */
/* WAL file header (64 bytes)                                           */
/* ------------------------------------------------------------------ */

struct wal_file_header {
    uint64_t magic;               /* HFSSS_FMT_MAGIC_WAL                       */
    uint32_t format_version;
    uint32_t flags;
    uint64_t associated_checkpoint_seq;
    uint64_t create_timestamp_ns;
    uint32_t record_count;        /* updated on clean close                    */
    uint32_t header_crc32;
    uint8_t  reserved[24];
} __attribute__((packed));

static_assert(sizeof(struct wal_file_header) == 64,
              "wal_file_header must be exactly 64 bytes");

/* ------------------------------------------------------------------ */
/* WAL record types                                                     */
/* ------------------------------------------------------------------ */

enum wal_record_type {
    WAL_REC_L2P_UPDATE       = 0x0001,
    WAL_REC_BLOCK_STATE      = 0x0002,
    WAL_REC_ERASE_COUNT      = 0x0003,
    WAL_REC_BAD_BLOCK        = 0x0004,
    WAL_REC_CHECKPOINT_BEGIN = 0x0010,
    WAL_REC_CHECKPOINT_END   = 0x0011,
    WAL_REC_SHUTDOWN_CLEAN   = 0x0020,
    WAL_REC_SHUTDOWN_PANIC   = 0x0021,
};

/* ------------------------------------------------------------------ */
/* WAL record (64 bytes, fixed)                                         */
/* ------------------------------------------------------------------ */

struct wal_record {
    uint64_t seq;                 /* monotonically increasing from 1           */
    uint32_t record_type;         /* enum wal_record_type                      */
    uint32_t payload_len;         /* <= 40                                     */
    uint8_t  payload[40];
    uint32_t record_crc32;        /* CRC32 of bytes 0–55                       */
    uint32_t end_marker;          /* must be HFSSS_WAL_END_MARKER              */
} __attribute__((packed));

static_assert(sizeof(struct wal_record) == 64,
              "wal_record must be exactly 64 bytes");

/* ------------------------------------------------------------------ */
/* WAL payload layouts                                                  */
/* ------------------------------------------------------------------ */

struct wal_payload_l2p {
    uint64_t lpn;
    uint64_t old_ppn;
    uint64_t new_ppn;
    uint64_t timestamp_ns;
    uint8_t  reserved[8];
} __attribute__((packed));

static_assert(sizeof(struct wal_payload_l2p) == 40,
              "wal_payload_l2p must be exactly 40 bytes");

struct wal_payload_block_state {
    uint8_t  channel;
    uint8_t  chip;
    uint8_t  die;
    uint8_t  plane;
    uint32_t block;
    uint8_t  old_state;
    uint8_t  new_state;
    uint8_t  reserved[30];
} __attribute__((packed));

static_assert(sizeof(struct wal_payload_block_state) == 40,
              "wal_payload_block_state must be exactly 40 bytes");

struct wal_payload_erase_count {
    uint8_t  channel;
    uint8_t  chip;
    uint8_t  die;
    uint8_t  plane;
    uint32_t block;
    uint32_t erase_count;
    uint8_t  reserved[28];
} __attribute__((packed));

static_assert(sizeof(struct wal_payload_erase_count) == 40,
              "wal_payload_erase_count must be exactly 40 bytes");

/* ------------------------------------------------------------------ */
/* Panic dump file header                                               */
/* ------------------------------------------------------------------ */

#define HFSSS_PANIC_MSG_LEN     256
#define HFSSS_PANIC_FRAMES      8

struct panic_dump_header {
    uint64_t magic;               /* HFSSS_FMT_MAGIC_PD                        */
    uint32_t format_version;
    uint32_t firmware_build_hash;
    uint64_t panic_timestamp_ns;
    uint32_t panic_reason_code;
    uint32_t panic_thread_id;
    char     panic_message[HFSSS_PANIC_MSG_LEN];
    uint64_t stack_frames[HFSSS_PANIC_FRAMES];
    uint64_t last_checkpoint_seq;
    uint64_t last_wal_seq;
    /* followed by variable-length partial L2P table summary */
} __attribute__((packed));

static_assert(sizeof(struct panic_dump_header) == 400,
              "panic_dump_header must be exactly 400 bytes");

#endif /* HFSSS_PERSISTENCE_FMT_H */
```

---

## 5. 函数接口详细设计

### 5.1 nand_file_open

打开或创建NAND数据文件，验证魔数与头部CRC。

```
nand_file_open(dev, ch, chip, die, plane, mode):
  1. 根据 ch/chip/die/plane 构造文件路径
  2. 若 mode=CREATE:
       分配并填充 nand_file_header 所有字段
       计算头部 CRC32（覆盖字节0-71）
       写入头部（80字节）
       以 0xFF 填充数据区至正确大小
  3. 若 mode=OPEN:
       读取头部（80字节）
       验证 magic == HFSSS_FMT_MAGIC_ND → 不符则返回 -ENOTSUP
       重新计算头部 CRC32 → 不符则返回 -EILSEQ
       验证 channel/chip/die/plane 与参数一致 → 不符则返回 -EINVAL
  4. 将 fd 与 header 存入设备上下文
  5. return 0
```

### 5.2 nand_file_write_page

将页数据与OOB写入NAND数据文件的对应偏移位置。

```
nand_file_write_page(dev, block, page, data, oob):
  1. offset = 80
             + block * pages_per_block * (page_size + oob_size)
             + page  * (page_size + oob_size)
  2. pwrite(fd, data, page_size, offset)
  3. pwrite(fd, oob,  oob_size,  offset + page_size)
  4. return 0（调用方负责 fdatasync 时机）
```

### 5.3 nand_file_read_page

从NAND数据文件读取页数据与OOB。

```
nand_file_read_page(dev, block, page, data, oob):
  1. offset = 同 nand_file_write_page 计算方式
  2. pread(fd, data, page_size, offset)
  3. pread(fd, oob,  oob_size,  offset + page_size)
  4. return 0
```

### 5.4 l2p_checkpoint_write

将完整L2P映射表写入检查点文件，写入完成后原子重命名至最终文件名。

```
l2p_checkpoint_write(mapping, seq, checkpoint_dir):
  1. 构造临时文件路径: <checkpoint_dir>/l2p_checkpoint_<seq>.tmp
  2. 打开文件，写入占位头部（64字节）
  3. 以流式写入 mapping->ppn[0..total_lpn_count-1]，同时累积 CRC64
  4. 回填头部所有字段（含 data_crc64_lo/hi）
  5. 计算并写入 header_crc32（覆盖字节0-55）
  6. fsync(fd); close(fd)
  7. rename(<seq>.tmp → l2p_checkpoint_<seq>.dat)  /* 原子操作 */
  8. 扫描目录，按序列号升序排列，删除超过3代的旧文件
  9. return seq
```

### 5.5 l2p_checkpoint_read

找到最新的有效检查点文件，验证CRC后将映射表加载至内存。

```
l2p_checkpoint_read(mapping, checkpoint_dir):
  1. 枚举 checkpoint_dir 下所有 l2p_checkpoint_*.dat 文件
  2. 按序列号降序排列，逐一尝试：
       a. 读取头部（64字节）
       b. 验证 magic == HFSSS_FMT_MAGIC_L2       → 失败则跳至下一文件
       c. 验证 header_crc32                        → 失败则跳至下一文件
       d. 验证 format_version 兼容性（见3.5节规则）→ 不兼容则返回错误
       e. 读取数据区，逐块累积 CRC64
       f. 验证 data_crc64 与头部字段一致           → 失败则跳至下一文件
       g. 将数据区复制至 mapping->ppn[]
       h. 返回检查点序列号
  3. 若所有文件均验证失败，返回 -ENODATA
```

### 5.6 wal_open

打开或创建WAL文件，写入或验证文件头。

```
wal_open(wal, wal_dir):
  1. 构造路径: <wal_dir>/hfsss.wal
  2. 若文件不存在（CREATE路径）:
       填充 wal_file_header（magic, version, checkpoint_seq, timestamp等）
       计算 header_crc32，写入头部（64字节）
       fdatasync(fd)
  3. 若文件已存在（OPEN路径）:
       读取头部（64字节）
       验证 magic == HFSSS_FMT_MAGIC_WAL
       验证 header_crc32
  4. 存储 fd 与头部至 wal 上下文
  5. return 0
```

### 5.7 wal_append

构造64字节WAL记录，计算CRC32，使用 pwrite 追加写入，并调用 fdatasync 保证持久性。

```
wal_append(wal, type, payload, len):
  1. 断言 len <= 40
  2. 构造 wal_record:
       .seq         = atomic_fetch_add(&wal->next_seq, 1)
       .record_type = type
       .payload_len = len
       .payload     = memcpy from payload（剩余字节清零）
       .end_marker  = HFSSS_WAL_END_MARKER
  3. 计算 CRC32（覆盖字节0-55）→ record.record_crc32
  4. offset = 64 + (record.seq - 1) * 64
     pwrite(fd, &record, 64, offset)
  5. fdatasync(fd)
  6. return 0
```

### 5.8 wal_replay

按序列号顺序读取全部WAL记录，将L2P更新应用至映射表；遇到不完整记录或CRC错误时停止。

```
wal_replay(wal, mapping):
  1. offset = 64  /* 跳过文件头 */
  2. loop:
       读取64字节 → buf
       若读取字节数 < 64: break  /* 不完整记录，正常结束 */
       验证 buf.end_marker == HFSSS_WAL_END_MARKER
           → 失败: 发出WARNING，break（不完整记录）
       计算 CRC32（覆盖字节0-55）
           → 与 buf.record_crc32 不符: 发出WARNING，break（记录N-1为最后有效记录）
       根据 buf.record_type 分发:
           WAL_REC_L2P_UPDATE:
               payload = (struct wal_payload_l2p*)buf.payload
               mapping->ppn[payload->lpn] = payload->new_ppn
           WAL_REC_BLOCK_STATE / WAL_REC_ERASE_COUNT / WAL_REC_BAD_BLOCK:
               转发至对应子系统处理函数
           WAL_REC_CHECKPOINT_END:
               记录最后完成的检查点序列号（后续截断依据）
           WAL_REC_SHUTDOWN_CLEAN:
               标记 clean_shutdown=true
       offset += 64
  3. return 已应用记录数
```

### 5.9 wal_truncate

成功完成检查点写入后，将WAL文件截断至仅保留文件头。

```
wal_truncate(wal):
  1. ftruncate(fd, 64)      /* 仅保留文件头 */
  2. 回写更新后的 wal_file_header（record_count=0, checkpoint_seq=new_seq）
  3. fdatasync(fd)
  4. wal->next_seq = 1      /* 重置序列号计数 */
  5. return 0
```

### 5.10 panic_dump_write

在崩溃处理路径中调用：打开Panic转储文件，写入所有字段，调用 fsync。

```
panic_dump_write(reason, msg, l2p_mapping):
  1. 获取当前纳秒时间戳 ts
  2. 构造路径: /var/hfsss/panic_dump_<ts>.bin
  3. 以 O_WRONLY|O_CREAT|O_TRUNC 打开文件（仅使用异步安全系统调用）
  4. 填充 panic_dump_header:
       magic, format_version, firmware_build_hash
       panic_timestamp_ns = ts
       panic_reason_code  = reason
       panic_thread_id    = gettid()
       panic_message      = strncpy(msg, HFSSS_PANIC_MSG_LEN)
       stack_frames[]     = backtrace 前8帧
       last_checkpoint_seq = 来自持久化上下文
       last_wal_seq        = 来自WAL上下文
  5. write(fd, &header, sizeof(header))
  6. 写入部分L2P摘要（前1000条与后1000条映射项）
  7. fsync(fd); close(fd)
```

---

## 6. 流程图

### 6.1 检查点写入流程

```
l2p_checkpoint_write()
    │
    ▼
打开临时文件 l2p_checkpoint_<seq>.tmp
    │
    ▼
流式写入 mapping->ppn[]，同步累积 CRC64
    │
    ▼
回填头部（含 data_crc64_lo/hi）
    │
    ▼
计算并写入 header_crc32
    │
    ▼
fsync(fd) → close(fd)
    │
    ▼
rename(.tmp → .dat)           /* 原子操作：对读者而言不存在中间状态 */
    │
    ▼
扫描目录，统计检查点文件数量
    │
    ├─ 数量 ≤ 3 ──► 无需删除
    │
    └─ 数量 > 3 ──► 删除序列号最小的旧文件
```

### 6.2 WAL追加写入流程

```
wal_append(type, payload, len)
    │
    ▼
构造 wal_record（64字节）
    │
    ▼
清零 payload 剩余字节；写入 end_marker=0xDEADBEEF
    │
    ▼
计算 CRC32（字节0-55）→ record.record_crc32
    │
    ▼
pwrite(fd, &record, 64, offset)
    │
    ▼
fdatasync(fd)                 /* 确保记录落盘，再向调用方返回 */
    │
    ▼
return 0
```

### 6.3 崩溃恢复流程

```
启动时恢复
    │
    ▼
l2p_checkpoint_read()
    ├─ 找到有效检查点 ──► 加载 mapping->ppn[]，记录 checkpoint_seq
    └─ 无有效检查点   ──► 初始化空映射表（全部 UNMAPPED）
    │
    ▼
wal_open(OPEN)
    ├─ WAL不存在   ──► 跳过重放，直接使用检查点状态
    └─ WAL已存在   ──► 继续执行重放
    │
    ▼
wal_replay(mapping)
    ├─ 按序列号顺序读取每条记录
    ├─ 验证 end_marker → 失败则停止
    ├─ 验证 CRC32      → 失败则停止（保留到上一条有效记录）
    └─ 应用 L2P 更新至 mapping->ppn[]
    │
    ▼
恢复完成，mapping 已是崩溃前最后一致状态
    │
    ▼
写入新 WAL_REC_CHECKPOINT_BEGIN 记录
    │
    ▼
继续正常运行
```

### 6.4 WAL记录完整性校验

```
读取64字节 buf
    │
    ▼
检查 buf[60..63] == 0xDEADBEEF？
    ├─ 否 ──► 发出 WARNING "incomplete WAL record at offset X"
    │         停止重放（截断标志）
    │
    ▼
重新计算 CRC32（buf[0..55]）
    │
    ├─ 与 buf[56..59] 不符 ──► 发出 WARNING "CRC mismatch at WAL seq Y"
    │                          停止重放
    │
    ▼
记录有效：按 record_type 应用
```

---

## 7. 容量与性能分析

| 指标 | 计算依据 | 结果 |
|------|----------|------|
| L2P检查点文件大小（2TB设备） | 512M LPN × 8字节/LPN | 4 GB/文件 |
| L2P检查点写入时间 | 4 GB ÷ 1 GB/s（主机NVMe顺序写） | ~4秒 |
| 3代检查点最大存储占用 | 4 GB × 3 | 12 GB |
| WAL单条记录大小 | 固定 | 64字节 |
| WAL写速率上限（1M IOPS，每次写操作产生1条记录） | 1M × 64字节 | 64 MB/s |
| NAND数据文件总大小（4TB NAND） | 4TB原始数据，存储于主机NVMe | 4 TB |
| WAL文件最大大小（检查点间隔1GB主机写，每4KB写产生1条记录） | 256K条 × 64字节 | 16 MB |
| 单次 wal_append 延迟（含 fdatasync） | 主机NVMe写延迟决定 | ~100µs |
| Panic转储文件大小 | 头部400字节 + 2000条LPN×8字节 | ~16 KB |

**关键约束说明**：

- L2P检查点写入期间模拟器持续运行；检查点写入采用写时快照语义（先写临时文件再 rename），不阻塞主I/O路径。
- fdatasync 调用频率与IOPS线性相关；高IOPS场景下建议将WAL目录挂载于低延迟NVMe设备。
- NAND数据文件采用稀疏文件（sparse file）布局；未写入页以0xFF填充，大多数文件系统会自动压缩。

---

## 8. 测试要点

| 测试ID | 测试描述 | 验证点 |
|--------|----------|--------|
| PF-001 | 写入L2P检查点后读回 | 所有LPN→PPN映射与写入前完全一致 |
| PF-002 | 头部CRC32损坏 | 加载时检测到损坏，返回 -EILSEQ，拒绝打开 |
| PF-003 | 数据区CRC64损坏 | 加载时拒绝当前文件，自动回退到上一代检查点 |
| PF-004 | WAL追加100万条记录后重放 | 所有记录已全部应用，映射结果与顺序写入一致 |
| PF-005 | WAL最后一条记录截断（文件末尾不足64字节） | 重放停止于最后完整记录，无错误崩溃 |
| PF-006 | WAL第N条记录CRC错误 | 重放停止于第N-1条，发出WARNING，返回已应用数量 |
| PF-007 | WAL记录 end_marker 非0xDEADBEEF | 视为不完整记录，停止重放，发出WARNING |
| PF-008 | NAND数据文件：写页后重新打开并读取 | 数据与OOB字段逐字节一致 |
| PF-009 | 未写入页OOB的LPN字段 | 读取到的 lpn == HFSSS_UNMAPPED（0xFFFFFFFFFFFFFFFF） |
| PF-010 | NAND文件头magic错误 | 打开时检测到魔数不符，返回 -ENOTSUP |
| PF-011 | Panic转储：触发Panic后验证文件 | 文件存在，magic正确，panic_timestamp_ns非零 |
| PF-012 | Panic转储：多次触发不覆盖旧文件 | 每次生成带独立时间戳的新文件，旧文件完整保留 |
| PF-013 | static_assert验证 | wal_record == 64字节，l2p_checkpoint_header == 64字节，nand_oob_entry == 384字节 |
| PF-014 | 版本兼容：打开未来版本写入的文件（writer_version > current） | 打开成功并发出WARNING，不崩溃，数据可读 |
| PF-015 | 版本兼容：打开要求更高最低读取版本的文件（current < min_reader） | 拒绝打开，返回明确的迁移提示错误码 |
| PF-016 | 检查点轮转：连续写入4次检查点 | 目录中仅保留最新3个文件，最旧的已被删除 |
| PF-017 | 崩溃恢复完整流程 | 写入检查点 → 追加WAL → 模拟崩溃 → 重启重放 → 映射一致 |
| PF-018 | WAL截断后验证 | truncate后文件大小 == 64字节，header字段record_count == 0 |
| PF-019 | NAND文件channel/die标识不一致 | 打开时验证失败，返回 -EINVAL |
| PF-020 | 并发写入多个NAND文件 | 16个Plane的文件并发写入，各文件内容独立、无交叉 |
| PF-021 | 大容量L2P检查点（2TB，512M条目） | 写入与读取均完成，CRC64验证通过，内存用量在预期范围内 |
| PF-022 | WAL文件不存在时的恢复路径 | 直接使用检查点状态，不报错，recovery流程正常结束 |

---

**文档统计**：
- 覆盖需求：REQ-131（主需求），REQ-051、REQ-052（隐式依赖）
- 新增头文件：`include/common/persistence_fmt.h`
- 相关源文件：`src/common/nand_file.c`、`src/common/l2p_checkpoint.c`、`src/common/wal.c`、`src/common/panic_dump.c`
- 函数接口数：20+
- 测试用例：22个
