# NAND/Media Command Coverage Enhancement Design

**Date:** 2026-04-10
**Repo baseline:** `origin/master` at `ee4604ce433f1fbb98f226725bcdfcb202fcf7f9`
**Scope:** NAND/Media layer only
**Priority direction:** Command coverage first
**Standards direction:** ONFI/Toggle standard-equivalent behavior first, vendor extensions later

---

## 1. Summary

This design upgrades the current NAND simulator from a basic `read/program/erase` latency model into a command-driven NAND/Media execution model that can represent modern enterprise TLC/QLC NAND behavior at the standard-command level.

The immediate goal is not vendor-specific fidelity. The goal is a robust standard core that:

- models ONFI/Toggle-equivalent command semantics explicitly
- supports suspend/resume behavior for long-running operations
- supports standard multi-plane operations before cache operations
- introduces a command/state execution model that later vendor profiles can extend

This design intentionally stops at the NAND/Media boundary. It does **not** plan HAL-to-FTL scheduling policy, NVMe exposure, or telemetry integration in this phase.

---

## 2. Current State

### 2.1 What exists today

The current implementation provides:

- basic NAND geometry and page/block hierarchy in [`include/media/nand.h`](../../../include/media/nand.h)
- a timing model with coarse `tR`, `tPROG`, and `tERS`-style latency APIs in [`src/media/timing.c`](../../../src/media/timing.c)
- direct `media_nand_read()`, `media_nand_program()`, and `media_nand_erase()` execution paths in [`src/media/media.c`](../../../src/media/media.c)
- a thin HAL NAND wrapper in [`include/hal/hal_nand.h`](../../../include/hal/hal_nand.h) and [`src/hal/hal_nand.c`](../../../src/hal/hal_nand.c)
- EAT tracking and reliability/error-count accounting

### 2.2 Current limitations

The current model does **not** provide:

- a die-level command state machine
- explicit command contexts for in-flight NAND commands
- standard `program suspend/resume` or `erase suspend/resume`
- standard `read id`, `read parameter page`, or `read status enhanced`
- standard multi-plane command execution
- standard cache read/program execution
- ONFI/Toggle stage timing semantics such as command/address/data-in/array-busy/data-out

### 2.3 Why the current shape is insufficient

The current model assumes one API call maps to one fully completed NAND operation. That works for basic functional tests, but it breaks down for:

- command interruption and resumption
- foreground read preemption during background program/erase
- standard status visibility while a command is in flight
- multi-plane legality rules
- future vendor/profile-driven behavior

Without an explicit command execution model, every advanced command becomes a one-off patch.

---

## 3. Standards and Industry Baseline

This design uses public standard and public vendor collateral as guidance, with the standard documents acting as the normative base.

### 3.1 Standards baseline

- **ONFI 4.2** is the minimum practical baseline for modern enterprise TLC/QLC command behavior and relaxed multi-plane constraints.
- **ONFI 5.x and 6.0** inform higher-speed interface evolution and more modern host/device interaction assumptions.
- **JESD230 ONFI/Toggle interoperability guidance** is used to define the Toggle-side “standard-equivalent” behavior rather than inventing a second execution model in phase 1.

### 3.2 Public industry signals used for prioritization

Public Micron, Kioxia, and Samsung enterprise NAND collateral consistently points toward:

- higher interface speed
- more plane-level/internal parallelism
- stronger enterprise QoS expectations

That makes the following behaviors high-value even before vendor-specific details are added:

- suspend/resume
- multi-plane execution
- explicit status observability
- profile-based timing and legality overrides

### 3.3 Scope decision from discussion

Per design discussion, the following decisions are fixed:

1. `read parameter page` is included in the first batch of foundational command coverage
2. `multi-plane` simulation is prioritized ahead of `cache read/program`
3. Toggle support in phase 1 is implemented as **standard-equivalent behavior**, not as a separate private opcode family

---

## 4. Goals and Non-Goals

### 4.1 Goals

- create a standard-command execution model for NAND/Media
- cover standard ONFI/Toggle-equivalent enterprise NAND commands that matter for TLC/QLC simulation
- make `program suspend/resume` and `erase suspend/resume` first-class behaviors
- add standard multi-plane simulation before cache command simulation
- preserve current basic functional behavior for existing read/program/erase users
- create clean hooks for future vendor profiles

### 4.2 Non-goals

- vendor-specific raw command table emulation in phase 1
- cycle-accurate waveform/bus simulation
- FTL scheduling policy for when to issue suspend/resume
- NVMe-visible command mapping for these NAND commands
- telemetry, CLI, or OOB surfacing of the new behaviors
- full read-retry or private feature-address emulation in phase 1

---

## 5. Proposed Architecture

The NAND/Media enhancement is split into three layers.

### 5.1 Standard Command Layer

This layer defines command types, legality, observable status, and command semantics. It does not directly mutate page data.

Responsibilities:

- define supported standard commands
- validate legal command issuance for current die state
- define which commands are interruptible
- define what status bits are observable at each stage
- normalize ONFI and Toggle-equivalent semantics into one internal command model

### 5.2 Media Execution Layer

This is the actual simulator engine.

Responsibilities:

- hold die/plane/channel execution state
- track in-flight command context
- manage suspend/resume
- advance EAT by execution stages instead of one-shot latency
- enforce multi-plane legality and resource occupancy
- update NAND data and metadata at the correct execution boundary

### 5.3 Vendor Extension Layer

This layer is deferred for full implementation, but the interface must exist now.

Responsibilities:

- define `nand_profile`
- override timing values
- override legality constraints
- advertise supported standard command subsets
- later host vendor-specific behaviors without reworking the core state machine

---

## 6. Standard Command Coverage Model

### 6.1 Priority tiering

#### P0: Foundational standard commands

- `read`
- `program`
- `erase`
- `reset`
- `read status`
- `read status enhanced`
- `read id`
- `read parameter page`
- `program suspend`
- `program resume`
- `erase suspend`
- `erase resume`

#### P1: Standard multi-plane commands

- `multi-plane read`
- `multi-plane program`
- `multi-plane erase`

#### P2: Standard cache commands

- `cache read`
- `cache program`

#### P3: Future standard/profile-adjacent commands

- `copyback read/program`
- `set feature`
- `get feature`
- read-retry-related profile behaviors

### 6.2 Why multi-plane comes before cache

The current simulator already models planes as explicit geometry objects, so multi-plane is the more structurally natural next capability. It also aligns better with public enterprise TLC/QLC collateral that emphasizes internal parallelism. Cache commands should come after the state machine and multi-plane legality rules are stable.

---

## 7. NAND State Model

### 7.1 Die-level execution states

Each die should expose an explicit execution state machine with at least:

- `DIE_IDLE`
- `DIE_READ_SETUP`
- `DIE_READ_ARRAY_BUSY`
- `DIE_READ_DATA_XFER`
- `DIE_PROG_SETUP`
- `DIE_PROG_ARRAY_BUSY`
- `DIE_ERASE_SETUP`
- `DIE_ERASE_ARRAY_BUSY`
- `DIE_SUSPENDED_PROG`
- `DIE_SUSPENDED_ERASE`
- `DIE_RESETTING`

### 7.2 Command context

Each in-flight command should record:

- opcode
- target address set
- start timestamp
- remaining execution time
- execution phase
- suspend count
- last suspend timestamp
- plane mask or target plane set
- result/status snapshot

### 7.3 Resource ownership

- **channel** owns command/address/data transfer serialization
- **die** owns array-busy state and suspend/resume eligibility
- **plane** owns multi-plane address legality and occupancy relationships

This prevents the current model’s oversimplification where a single latency update substitutes for all resource interactions.

---

## 8. Suspend/Resume Semantics

### 8.1 Standard behavior to model first

- `program suspend` is valid only during `DIE_PROG_ARRAY_BUSY`
- `erase suspend` is valid only during `DIE_ERASE_ARRAY_BUSY`
- successful suspend moves the die to `DIE_SUSPENDED_PROG` or `DIE_SUSPENDED_ERASE`
- a `read` may execute while the die is in a suspended program/erase state
- a `resume` is only legal for the matching suspended command type
- `reset` aborts any in-flight command and returns the die to a clean state

### 8.2 Timing implications

Suspend/resume must not be emulated as full command restart. The execution model must preserve:

- elapsed command time
- remaining array-busy time
- suspend overhead
- resume overhead

This is the minimum needed for believable enterprise NAND behavior.

### 8.3 Status visibility

The status path must expose at least:

- ready/busy
- suspended
- pass/fail completion state
- reset-in-progress or transient busy where relevant

`read status enhanced` should be the main structured window into the internal command state.

---

## 9. Timing Model Upgrade

The current timing model in [`src/media/timing.c`](../../../src/media/timing.c) provides only coarse operation latency. That is not enough for command-stage modeling.

The timing model should be extended from:

- one latency per read/program/erase

to:

- command input timing
- address input timing
- write-buffer / data-latch timing
- array-busy timing
- resume timing
- read-after-program / read-after-suspend timing
- cross-command setup timing such as `tCCS`

The design does **not** require cycle-accurate bus simulation. It does require phase-aware timing so that:

- suspend/resume changes latency in a realistic way
- multi-plane commands do not look identical to serial single-plane commands
- future profiles can override meaningful timing points

---

## 10. Profile Model

Phase 1 should not implement vendor-private command behavior, but it should define profile structure now.

Suggested profile dimensions:

- `interface_family`: `onfi` or `toggle_equivalent`
- `nand_class`: `enterprise_tlc` or `enterprise_qlc`
- supported standard command bitmap
- timing table
- multi-plane legality rules
- suspend/resume timing constants
- parameter-page identity and capability advertisement

Initial generic profiles:

- `generic_onfi_enterprise_tlc`
- `generic_onfi_enterprise_qlc`
- `generic_toggle_enterprise_tlc`
- `generic_toggle_enterprise_qlc`

These should all share the same command engine and differ only in declarative capability/timing tables unless a later phase proves a real behavioral split is necessary.

---

## 11. Phase Plan

### Phase 0: Capability Matrix and Spec Freeze

Deliverables:

- standard command coverage matrix
- current-vs-target gap table
- per-command priority and defer decisions

Exit criteria:

- every target command is classified as `P0/P1/P2/P3`
- every deferred command has an explicit reason

### Phase 1: Command Engine Skeleton

Deliverables:

- die command state machine
- in-flight command context objects
- execution-phase-aware EAT updates
- compatibility wrapper path for current `media_nand_*` entry points

Exit criteria:

- existing `read/program/erase/reset/status` flow through the new engine
- existing functional tests still pass with no intended behavior regressions

### Phase 2: Foundational Standard Commands

Deliverables:

- `read status`
- `read status enhanced`
- `read id`
- `read parameter page`
- `reset`
- legality and status reporting core

Exit criteria:

- busy/idle/resetting/suspended states produce deterministic status outputs

### Phase 3: Suspend/Resume

Deliverables:

- `program suspend/resume`
- `erase suspend/resume`
- remaining-time accounting
- suspend/resume overhead timing

Exit criteria:

- suspended program/erase can be interrupted by read
- resume continues the interrupted command without full restart

### Phase 4: Multi-Plane Simulation

Deliverables:

- `multi-plane read/program/erase`
- plane legality checks
- ONFI 4.2-aware relaxed-addressing rule support where applicable

Exit criteria:

- legal multi-plane commands succeed
- illegal plane combinations fail deterministically
- timing/resource occupancy differs from serial single-plane execution

### Phase 5: Cache Command Simulation

Deliverables:

- `cache read`
- `cache program`
- stage-aware overlap rules

Exit criteria:

- cache command behavior is observably distinct from plain command behavior

### Phase 6: Profile Hooks

Deliverables:

- profile table format
- generic ONFI and generic Toggle-equivalent profiles
- timing/legality override infrastructure

Exit criteria:

- command engine selects behavior from profile instead of hard-coded global assumptions

### Phase 7: Validation Expansion

Deliverables:

- unit tests for every new command
- state-machine legality tests
- suspend/resume timing tests
- multi-plane legality matrix tests
- compatibility regression tests for existing media behavior

Exit criteria:

- each command has success, illegal-state, and edge-case tests
- standard profiles are covered by at least one focused validation suite

---

## 12. Validation Strategy

### 12.1 Test categories

- **command semantic tests**: one command, one expected behavior
- **state-machine tests**: illegal transition and recovery behavior
- **timing tests**: remaining time, suspend cost, resume cost
- **resource tests**: plane/channel/die occupancy conflicts
- **compatibility tests**: existing media/hal tests still behave as expected

### 12.2 Must-have behavior checks

- suspend only succeeds in legal busy phases
- resume only succeeds for matching suspended command type
- reset aborts in-flight commands cleanly
- read during suspended program/erase is legal
- read during non-suspended program/erase is rejected or deferred according to model
- multi-plane legality is deterministic and profile-aware
- `read parameter page` and `read id` expose profile-consistent identity

---

## 13. Risks and Mitigations

### Risk 1: Overbuilding toward cycle-accurate simulation

Mitigation:

- keep phase timing coarse but stage-aware
- do not model electrical waveform details

### Risk 2: Vendor-specific behavior leaks into the standard core too early

Mitigation:

- keep all vendor/profile differences declarative
- do not add private opcodes in phase 1

### Risk 3: Existing media/FTL code assumes immediate completion

Mitigation:

- keep wrapper compatibility in early phases
- preserve the synchronous external behavior of `media_nand_read/program/erase` until callers are intentionally updated

### Risk 4: Multi-plane rules get implemented against an outdated assumption set

Mitigation:

- anchor rule tables to ONFI 4.2+ baseline and make them profile-overridable

---

## 14. Open Decisions Already Resolved

The following decisions are now fixed for this design:

- `read parameter page` is included in the foundational command set
- `multi-plane` is implemented before `cache read/program`
- Toggle support is modeled as standard-equivalent capability in phase 1

---

## 15. References

- ONFI specs page: <https://onfi.org/specs.html>
- ONFI 6.0 final PDF: <https://onfi.org/files/ONFI_6_0_Final.pdf>
- JESD230 interoperability PDF hosted by ONFI: <https://onfi.org/files/jesd230c-nov2016.pdf>
- Micron G9 NAND overview: <https://www.micron.com/products/storage/nand-flash/g9-nand>
- Micron G9 QLC NAND overview: <https://www.micron.com/products/storage/nand-flash/qlc-nand/g9-qlc-nand>
- Kioxia enterprise SSD built with BiCS8 TLC: <https://americas.kioxia.com/en-us/business/news/2025/ssd-20250515-2.html>
- Kioxia 9th-gen BiCS FLASH TLC announcement: <https://www.kioxia.com/en-jp/about/news/2025/20250725-1.html>
- Kioxia next-gen 4.8Gb/s Toggle DDR6.0 announcement: <https://www.kioxia.com/en-jp/business/news/2025/20250220-1.html>
- Samsung 9th-gen TLC V-NAND announcement: <https://news.samsung.com/global/samsung-electronics-begins-industrys-first-mass-production-of-9th-gen-v-nand>
- Samsung 9th-gen QLC V-NAND announcement: <https://news.samsung.com/global/samsung-begins-industrys-first-mass-production-of-qlc-9th-gen-v-nand-for-ai-era>
