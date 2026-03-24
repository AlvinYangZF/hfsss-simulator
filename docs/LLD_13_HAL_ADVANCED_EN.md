# HAL Advanced Features Low-Level Design

## Revision History

| Version | Date       | Author | Description     |
|---------|------------|--------|-----------------|
| V1.0    | 2026-03-15 | HFSSS  | Initial release |

## Table of Contents

1. [Module Overview](#1-module-overview)
2. [Requirements Traceability](#2-requirements-traceability)
3. [Data Structure Design](#3-data-structure-design)
4. [Header File Design](#4-header-file-design)
5. [Function Interface Design](#5-function-interface-design)
6. [Flow Diagrams](#6-flow-diagrams)
7. [Integration Notes](#7-integration-notes)
8. [Test Plan](#8-test-plan)

---

## 1. Module Overview

The HAL Advanced module extends the base HAL defined in LLD_04_HAL.md, covering three advanced capability domains: NVMe asynchronous event mechanism, PCIe link state management, and PCI configuration space management.

**REQ-063: Async Event Management (AER)**: NVMe specification defines the Asynchronous Event Request command mechanism, allowing the host to be asynchronously notified when important controller state changes occur. The controller maintains a pending event queue and outstanding AER command queue; when an event occurs, if an outstanding AER command exists it is completed immediately, otherwise the event is cached in a ring queue.

**REQ-064: PCIe Link State Management**: PCIe link supports L0 (active), L0s (fast standby), L1 (low-power standby), L2 (powered down) states, plus Hot Reset and Function Level Reset (FLR). ASPM policy controls which low-power states can be entered automatically.

**REQ-069: PCI Management**: PCI configuration space provides device identification, BAR mapping, and capability linked lists. This module provides byte/word/dword read/write interfaces with read-only field protection.

**Requirements Coverage**: REQ-063, REQ-064, REQ-069.

---

## 2. Requirements Traceability

| Req ID  | Description | Priority | Target |
|---------|-------------|----------|--------|
| REQ-063 | NVMe AER command queue; event types (Error, SMART/Health, Notice, NVM CS); host notification | P0 | V2.0 |
| REQ-064 | PCIe link states (L0/L0s/L1/L2); ASPM policy; Hot Reset and FLR | P0 | V2.0 |
| REQ-069 | PCI config space R/W; BAR management; capability list traversal | P0 | V2.0 |

---

## 3. Data Structure Design

```c
// ====== REQ-063: Async Event Management ======

enum nvme_async_event_type {
    NVME_AER_TYPE_ERROR = 0, NVME_AER_TYPE_SMART_HEALTH = 1,
    NVME_AER_TYPE_NOTICE = 2, NVME_AER_TYPE_NVM_CMD_SET = 6,
};

enum nvme_async_event_info {
    NVME_AEI_ERROR_WRITE_TO_INVALID_DOORBELL = 0x00,
    NVME_AEI_SMART_NVM_SUBSYSTEM_RELIABILITY = 0x00,
    NVME_AEI_SMART_TEMPERATURE_THRESHOLD = 0x01,
    NVME_AEI_SMART_SPARE_BELOW_THRESHOLD = 0x02,
    NVME_AEI_NOTICE_NS_ATTRIBUTE_CHANGED = 0x00,
    NVME_AEI_NOTICE_FW_ACTIVATION_STARTING = 0x01,
};

#define AER_PENDING_MAX  16
#define AER_REQUEST_MAX  16

struct hal_aer_ctx {
    struct nvme_aer_pending pending[AER_PENDING_MAX];
    uint32_t pending_head, pending_tail;
    uint16_t outstanding_cids[AER_REQUEST_MAX];
    uint32_t outstanding_count;
    pthread_mutex_t lock;
};

// ====== REQ-064: PCIe Link State ======

enum pcie_link_state {
    PCIE_LINK_L0 = 0, PCIE_LINK_L0s, PCIE_LINK_L1,
    PCIE_LINK_L2, PCIE_LINK_RESET, PCIE_LINK_FLR,
};

enum pcie_aspm_policy { ASPM_DISABLED=0, ASPM_L0s, ASPM_L1, ASPM_L0s_L1 };

struct pcie_link_ctx {
    enum pcie_link_state state, prev_state;
    enum pcie_aspm_policy aspm_policy;
    uint64_t state_enter_ns;
    uint32_t l0s_exit_latency_us;   /* 10-200 us */
    uint32_t l1_exit_latency_us;    /* 1000-65000 us */
    uint32_t l2_recovery_ms;
    bool     flr_in_progress;
    pthread_mutex_t lock;
};

// ====== REQ-069: PCI Management ======

#define PCI_CFG_SPACE_SIZE      256
#define PCI_EXT_CFG_SPACE_SIZE  4096

struct hal_pci_cfg {
    uint8_t  cfg_space[PCI_CFG_SPACE_SIZE];
    uint8_t  ext_cfg_space[PCI_EXT_CFG_SPACE_SIZE];
    uint32_t bar_base[6], bar_size[6];
    bool     bar_is_mmio[6];
    uint16_t vendor_id;       /* 0x1B36 */
    uint16_t device_id;       /* 0x0010 */
    uint8_t  revision_id;
    uint8_t  class_code[3];   /* 0x01, 0x08, 0x02 = NVMe */
};
```

---

## 4. Header File Design

```c
// include/hal/hal_advanced.h
#ifndef HFSSS_HAL_ADVANCED_H
#define HFSSS_HAL_ADVANCED_H

/* AER */
int hal_aer_init(struct hal_aer_ctx *ctx);
int hal_aer_submit_request(struct hal_aer_ctx *ctx, uint16_t cid);
int hal_aer_post_event(struct hal_aer_ctx *ctx, enum nvme_async_event_type type,
                       enum nvme_async_event_info info, uint8_t log_page_id);
int hal_aer_complete_event(struct hal_aer_ctx *ctx, uint16_t cid,
                           enum nvme_async_event_type type,
                           enum nvme_async_event_info info, uint8_t log_page_id);
int hal_aer_abort_pending(struct hal_aer_ctx *ctx);

/* PCIe Link */
int pcie_link_transition(struct pcie_link_ctx *ctx, enum pcie_link_state new_state);
int pcie_link_enter_l0s(struct pcie_link_ctx *ctx);
int pcie_link_exit_l0s(struct pcie_link_ctx *ctx);
int pcie_link_enter_l1(struct pcie_link_ctx *ctx);
int pcie_link_exit_l1(struct pcie_link_ctx *ctx);
int pcie_hot_reset(struct pcie_link_ctx *ctx);
int pcie_flr(struct pcie_link_ctx *ctx);

/* PCI Config */
uint8_t  hal_pci_cfg_read8 (const struct hal_pci_cfg *cfg, uint32_t offset);
uint16_t hal_pci_cfg_read16(const struct hal_pci_cfg *cfg, uint32_t offset);
uint32_t hal_pci_cfg_read32(const struct hal_pci_cfg *cfg, uint32_t offset);
int hal_pci_cfg_write8 (struct hal_pci_cfg *cfg, uint32_t offset, uint8_t  val);
int hal_pci_cfg_write16(struct hal_pci_cfg *cfg, uint32_t offset, uint16_t val);
int hal_pci_cfg_write32(struct hal_pci_cfg *cfg, uint32_t offset, uint32_t val);
int hal_pci_bar_map(struct hal_pci_cfg *cfg, uint8_t bar_idx, uint32_t base, uint32_t size);
uint8_t hal_pci_capability_find(const struct hal_pci_cfg *cfg, uint8_t cap_id);

#endif
```

---

## 5. Function Interface Design

### 5.1 AER Functions

- **hal_aer_submit_request**: If pending events exist, complete immediately; otherwise queue CID in outstanding array. Returns -ENOBUFS if max outstanding reached.
- **hal_aer_post_event**: If outstanding CID exists, complete it; otherwise buffer event in pending ring. Returns -ENOSPC if pending ring full.
- **hal_aer_complete_event**: Constructs AER completion DW0 (bits[2:0]=type, bits[15:8]=info, bits[23:16]=log_page_id), posts to Admin CQ, triggers MSI-X vector 0.
- **hal_aer_abort_pending**: On controller reset, aborts all outstanding AER commands with SC=0x07.

### 5.2 PCIe Link State Management

Legal transition matrix:

| Current | Allowed Targets |
|---------|----------------|
| L0 | L0s, L1, RESET, FLR |
| L0s | L0, L1 |
| L1 | L0, L2 |
| L2 | L0 |
| RESET | L0 |
| FLR | L0 |

- **pcie_hot_reset**: Quiesce in-flight ops (100ms timeout), reset NVMe registers to defaults, preserve PCI config space, restore L0.
- **pcie_flr**: Save BAR/config snapshot, reset all controller state, restore BAR/config, transition to L0, notify host via MSI-X.

### 5.3 PCI Config Space

- Read: Returns 0xFF/0xFFFF/0xFFFFFFFF for out-of-range offsets. Unaligned read16/read32 returns all-1s with warning.
- Write: Returns -EACCES for read-only fields (VID offset 0x00-0x01, DID 0x02-0x03, RevID 0x08, Class Code 0x09-0x0B).
- bar_map: Validates bar_idx 0-5 and power-of-2 size.
- capability_find: Walks linked list from offset 0x34, max 48 iterations to prevent infinite loops.

---

## 6. Flow Diagrams

### 6.1 AER Lifecycle

**Scenario A**: Host submits AER first, event occurs later -> outstanding CID consumed, immediate completion.

**Scenario B**: Event occurs first, host submits AER later -> event buffered in pending ring, completed on next AER submit.

### 6.2 PCIe Link State Machine

```
L0 (Active) <-> L0s (Standby, <200us exit)
L0 <-> L1 (Low Power, 1-65ms exit)
L1 -> L2 (Power Off) -> L0 (recovery)
Any -> RESET -> L0
L0 -> FLR -> L0
```

### 6.3 FLR Sequence

```
Detect FLR -> Save BAR/config snapshot -> transition(FLR)
  -> Reset all SQ/CQ/registers/DMA -> Restore BAR/config
  -> transition(L0) -> MSI-X notify host
```

---

## 7. Integration Notes

### 7.1 AER Event Trigger Sources

| Trigger | Call | Log Page |
|---------|------|----------|
| Temperature exceeds threshold | hal_aer_post_event(SMART_HEALTH, TEMP, 0x02) | SMART Log |
| Spare below threshold | hal_aer_post_event(SMART_HEALTH, SPARE, 0x02) | SMART Log |
| Namespace attribute changed | hal_aer_post_event(NOTICE, NS_ATTR, 0x04) | Changed NS List |
| Firmware activation starting | hal_aer_post_event(NOTICE, FW_ACTIVATION, 0x03) | FW Slot Info |

### 7.2 PCIe Link Trigger Sources

| Trigger | Call |
|---------|------|
| NVMe Power State PS3 | pcie_link_enter_l1() |
| HAL watchdog timeout | pcie_flr() |
| QEMU bus reset | pcie_hot_reset() |

---

## 8. Test Plan

| Test ID | Description | Expected Result |
|---------|-------------|----------------|
| HA-001 | AER submit then event | Completion within 1ms; type/info/log_page correct |
| HA-002 | Event then AER submit | Pending event consumed; immediate completion |
| HA-003 | 17th pending event overflow | Returns -ENOSPC; first 16 events intact |
| HA-004 | 17th outstanding AER | Returns -ENOBUFS |
| HA-005 | Controller reset aborts 3 AER | 3 completions with SC=0x07 |
| HA-006 | AER DW0 format | Bits[2:0]=type, [15:8]=info, [23:16]=log_page_id |
| HA-007 | Concurrent AER post/submit | No deadlock; Helgrind clean |
| HA-008 | L0->L0s->L0 transition | Exit latency 10-200us; state logged |
| HA-009 | L0->L1->L0 transition | Exit latency 1-65ms |
| HA-010 | Illegal L0s->L2 transition | Returns -EINVAL; state unchanged |
| HA-011 | Hot Reset with in-flight commands | Commands aborted; NVMe registers reset; BAR preserved |
| HA-012 | FLR controller state cleared | SQ/CQ zero; BAR unchanged; host can re-init |
| HA-013 | PCI config read VID/DID | VID=0x1B36, DID=0x0010, Class=0x01/0x08/0x02 |
| HA-014 | Write to VID (read-only) | Returns -EACCES; VID unchanged |
| HA-015 | Unaligned read16 | Returns 0xFFFF; warning logged |
| HA-016 | BAR map with non-power-of-2 size | Returns -EINVAL |
| HA-017 | Capability find PCIe (0x10) | Returns correct offset |
| HA-018 | Capability find non-existent (0xFF) | Returns 0; <= 48 iterations |

---

**Document Statistics**:
- Requirements covered: 3 (REQ-063, REQ-064, REQ-069)
- Header file: `include/hal/hal_advanced.h`
- Function interfaces: 20
- Test cases: 18 (HA-001 through HA-018)

## Appendix: Cross-References

| Reference | Document |
|-----------|----------|
| Temperature threshold AER | LLD_07_OOB_MANAGEMENT |
| NVMe register handling | LLD_16_KERNEL_MODULE |
| Thermal throttle triggers | LLD_12_REALTIME_SERVICES |
