# Enterprise Controller Simulator Enhancement Design

**Date:** 2026-04-11  
**Repo baseline:** `origin/master` at `4155575d1d7e4af8f71db8ae30d74711f63d7fe1`  
**Scope:** controller simulator and host-facing enterprise behavior  
**Priority direction:** public enterprise-controller capability first, vendor-private fidelity later  
**Input sources:** Marvell Bravera SC5 research, Silicon Motion SM8366 research, InnoGrit Tacoma IG5669 research

---

## 1. Summary

This design upgrades HFSSS from a basic single-controller NVMe simulation into a configurable **enterprise SSD controller simulator** informed by three public PCIe Gen5 controller families:

- Marvell **Bravera SC5** (`Vail` treated only as a likely codename)
- Silicon Motion **SM8366**
- InnoGrit **Tacoma IG5669**

The goal is **not** to reverse-engineer vendor firmware. The goal is to extract a credible public capability superset and turn it into a reusable controller model that better reflects modern enterprise SSD behavior.

The target simulator should support:

- multiple enterprise controller personas rather than one default controller shape
- stronger queue arbitration, namespace isolation, and QoS behavior
- controller modes such as conventional NVMe, ZNS, and FDP-oriented behavior
- tighter coupling between controller profile and NAND/media profile
- richer telemetry, security lifecycle, and maintenance interference modeling

This design intentionally stays at the **controller / host-visible behavior** level. It does not attempt vendor-private opcode fidelity or cycle-accurate silicon modeling.

---

## 2. Current State

### 2.1 What exists today

HFSSS already has a useful controller/NVMe foundation:

- controller context and configuration in [`include/controller/controller.h`](../../../include/controller/controller.h) and [`src/controller/controller.c`](../../../src/controller/controller.c)
- arbitration and scheduling skeletons in [`include/controller/arbiter.h`](../../../include/controller/arbiter.h) and [`src/controller/scheduler.c`](../../../src/controller/scheduler.c)
- DWRR-style QoS structures in [`include/controller/qos.h`](../../../include/controller/qos.h)
- security primitives in [`include/controller/security.h`](../../../include/controller/security.h)
- telemetry building blocks in [`include/common/telemetry.h`](../../../include/common/telemetry.h)
- NVMe opcode and register surface in [`include/pcie/nvme.h`](../../../include/pcie/nvme.h)
- user-space NVMe device emulation in [`src/pcie/nvme_uspace.c`](../../../src/pcie/nvme_uspace.c)

### 2.2 Current limitations

The current controller shape is still much closer to a single generic SSD model than to an enterprise controller family:

- no explicit **controller profile** abstraction
- no first-class distinction between 8-channel, 16-channel, or 16/18-channel controller personas
- no **dual-port** host model
- no PF/VF or tenant-partitioning model
- scheduler behavior is materially simpler than public enterprise QoS / arbiter claims
- no controller-level **ZNS / FDP / SEF / Open Channel** persona split
- no controller-level **atomic write** or offload abstraction
- telemetry, QoS, security, and NAND/media timing are present as separate subsystems rather than one coherent enterprise-controller behavior

### 2.3 Related design work already in repo

This design should be read alongside:

- [`2026-04-10-nand-media-command-coverage-design.md`](2026-04-10-nand-media-command-coverage-design.md)

The NAND/media design handles command-state and media-side realism. This controller design handles **host-facing policy, persona, and enterprise feature behavior**.

---

## 3. Public Capability Baseline

### 3.1 Cross-vendor common signals

Across the three public controller families, the following common signals are strong enough to drive a simulator design:

- PCIe **Gen5 x4** is the baseline host link
- high NAND parallelism is normal: **8 / 16 / 16-18 channels**
- enterprise controllers expose stronger **QoS, latency isolation, and namespace behavior**
- media and controller behavior are increasingly coupled through **high-speed ONFI/Toggle timing**
- enterprise deployment modes now routinely include **ZNS** and other placement-aware behavior
- security is controller-facing, not just media-facing:
  - secure boot
  - root of trust
  - encryption lifecycle
  - firmware protection
- telemetry and observability are expected controller features

### 3.2 Vendor-specific emphasis

- **Marvell Bravera SC5**
  - strongest inputs: 8/16-channel split, dual-port, PF/VF-like partitioning, SLA/QoS, SEF/ZNS/Open Channel flexibility

- **Silicon Motion SM8366**
  - strongest inputs: 16-channel enterprise profile, 1024 queue pairs, 128 namespaces, PerformaShape, FDP, host-based FTL cooperation

- **InnoGrit Tacoma IG5669**
  - strongest inputs: 16/18-channel coupling to NAND/media profile, ZNS, atomic write, low-latency persona, optional offload behavior

### 3.3 What is explicitly out of scope

Public documents do **not** reveal enough detail to justify:

- vendor-private registers or admin opcodes
- exact telemetry page formats
- exact FTL algorithms
- exact latency distributions under every workload
- private firmware recovery state machines

Those should remain abstract or deferred.

---

## 4. Goals And Non-Goals

### 4.1 Goals

- introduce a first-class **controller profile** abstraction
- make controller behavior configurable by enterprise persona instead of one generic default
- improve namespace-aware scheduling, arbitration, and maintenance interference behavior
- add host-visible enterprise mode differences such as ZNS and FDP-oriented behavior
- couple controller timing and policy more tightly to NAND/media profile
- add controller telemetry and security lifecycle modeling that is coherent at the persona level
- preserve a compatible baseline path for existing NVMe and controller tests where enterprise features are not enabled

### 4.2 Non-goals

- exact reproduction of Marvell, Silicon Motion, or InnoGrit firmware internals
- vendor-private command-set emulation in the first implementation
- SR-IOV, PF/VF, or computational-storage API fidelity beyond public behavior classes
- cycle-accurate host interface timing
- full cloud-SSD software stack emulation outside the controller boundary

---

## 5. Proposed Architecture

The enhanced simulator should split controller behavior into five layers.

### 5.1 Controller Profile Layer

This layer defines the public controller persona.

Responsibilities:

- define controller class such as:
  - `gen5_enterprise_8ch`
  - `gen5_enterprise_16ch`
  - `gen5_enterprise_16_18ch`
- define host-facing capability surface:
  - NVMe version persona
  - queue/namespace scale
  - dual-port support
  - ZNS/FDP/atomic-write availability
- define power, latency, and throughput envelopes

### 5.2 Enterprise Isolation Layer

This layer models how the controller separates workloads and controls latency.

Responsibilities:

- queue arbitration and dispatch policy
- namespace-aware QoS
- latency target enforcement
- multi-tenant isolation hooks
- maintenance interference accounting

### 5.3 Mode And Placement Layer

This layer represents controller-visible storage modes.

Responsibilities:

- conventional namespace mode
- ZNS mode
- FDP-oriented placement behavior
- later extension for SEF / Open Channel
- atomic write capability and failure behavior

### 5.4 Media Coupling Layer

This layer connects controller persona to NAND/media capabilities instead of treating media as interchangeable.

Responsibilities:

- bind controller profile to NAND/media profile
- vary controller timing by NAND interface generation and media class
- express how channel count, ONFI/Toggle generation, and media type affect controller-level performance and latency

### 5.5 Enterprise Lifecycle Layer

This layer owns controller state that matters to operations and security.

Responsibilities:

- secure boot and firmware protection state
- key-management and encryption lifecycle
- telemetry and health-state reporting
- maintenance-state visibility
- optional offload / in-storage-compute abstraction in later phases

---

## 6. Controller Capability Model

### 6.1 Baseline controller profile fields

The first controller-profile abstraction should at minimum define:

- `profile_name`
- `pcie_gen`
- `pcie_lane_width`
- `nvme_version_persona`
- `channel_count`
- `namespace_limit`
- `queue_pair_limit`
- `supports_dual_port`
- `supports_zns`
- `supports_fdp`
- `supports_atomic_write`
- `supports_pf_vf_partitioning`
- `supports_offload`
- `latency_envelope`
- `bandwidth_envelope`
- `power_envelope`
- `telemetry_class`
- `security_class`
- `nand_media_profile_ref`

### 6.2 Initial persona set

The first wave should define generic personas rather than vendor-branded replicas:

- `generic_gen5_enterprise_8ch`
- `generic_gen5_enterprise_16ch`
- `generic_gen5_enterprise_16_18ch`
- `generic_gen5_enterprise_16ch_dual_port`
- `generic_gen5_enterprise_zns`
- `generic_gen5_enterprise_fdp`

Vendor-inspired presets can then map onto these generic personas:

- Marvell-inspired profile family
- SM8366-inspired profile family
- Tacoma-inspired profile family

### 6.3 Why generic personas come first

The public data is sufficient to justify **behavior classes**, but not sufficient to justify faithful vendor replication. Generic personas avoid fake precision while still letting HFSSS simulate meaningful enterprise-controller differences.

---

## 7. Enhancement Themes

### 7.1 Scheduling And QoS

HFSSS already has DWRR and namespace QoS scaffolding, but this should become a persona-level capability rather than an isolated module.

The simulator should model:

- queue-depth pressure at controller scale
- namespace-aware arbitration under mixed load
- SLA-oriented latency shaping
- maintenance interference on tail latency
- vendor-inspired differences in aggressiveness and fairness

Primary public inputs:

- Marvell Elastic SLA Enforcer and multi-tenant QoS
- SM8366 PerformaShape and large queue/namespace scale
- Tacoma low-latency positioning

### 7.2 Dual-Port And Tenant Isolation

Dual-port is public for Marvell and SM8366. Tenant isolation is strongly implied by PF/VF and namespace/QoS claims.

The simulator should therefore support:

- optional dual-port host persona
- queue ownership and contention between host ports
- namespace and controller-resource partitioning
- later PF/VF-style partition abstraction

### 7.3 Mode And Placement Personas

Controller mode differences should be explicit, not hidden inside namespace metadata.

Required early behaviors:

- conventional namespace mode
- ZNS persona
- FDP-oriented placement persona

Deferred behaviors:

- SEF
- Open Channel
- host-based FTL cooperation beyond a coarse abstraction

### 7.4 Media-Coupled Controller Timing

Tacoma and SM8366 both point toward strong controller/media coupling. That means controller timing should not be static.

The enhanced controller simulator should vary:

- bandwidth saturation point
- queue-service latency
- background-maintenance impact
- atomic-write and completion timing

based on:

- channel count
- NAND interface generation
- TLC / QLC / SCM mode
- media-side command capability from the NAND/media design

### 7.5 Security And Telemetry Lifecycle

Security and telemetry should be modeled as a controller persona, not just as isolated helper libraries.

The model should expose:

- secure boot state
- firmware authenticity / versioning state
- encryption enabled / disabled / key-rotation state
- health and telemetry counters
- maintenance and reliability event visibility

---

## 8. Phase Plan

### Phase 0: Capability Matrix And Persona Freeze

Deliverables:

- controller capability matrix derived from the three research documents
- generic enterprise persona definitions
- mapping from vendor-inspired inputs to generic personas

Exit criteria:

- every proposed capability is marked as:
  - baseline
  - optional
  - deferred
- each initial persona has a stable field set

### Phase 1: Controller Profile Foundation

Deliverables:

- controller profile abstraction
- generic 8ch / 16ch / 16-18ch persona presets
- controller configuration path updated to consume a profile instead of only flat defaults
- compatibility wrapper path for existing controller startup

Exit criteria:

- current default behavior remains available through a baseline profile
- channel count, queue scale, and enterprise capability flags come from the active profile
- existing controller and NVMe tests still pass in baseline mode

### Phase 2: Enterprise QoS And Telemetry

Deliverables:

- stronger namespace-aware scheduling behavior
- persona-aware latency/QoS policy layer
- telemetry counters tied to queueing and maintenance state
- maintenance interference visibility

Exit criteria:

- mixed-load behavior differs across controller personas in a deterministic, testable way
- telemetry reflects queue pressure, latency, and maintenance state

### Phase 3: Mode And Placement Personas

Deliverables:

- conventional mode and ZNS persona split
- FDP-oriented placement abstraction
- profile-driven namespace/placement policy behavior

Exit criteria:

- the active persona can alter host-visible behavior beyond peak throughput
- ZNS and FDP behaviors are profile-driven, not ad hoc

### Phase 4: Dual-Port And Enterprise Lifecycle

Deliverables:

- dual-port host model
- enterprise security lifecycle state machine
- firmware protection and key-lifecycle state

Exit criteria:

- dual-port contention and failover behavior are testable
- security state affects controller behavior in deterministic ways

### Phase 5: Advanced Enterprise Extensions

Deliverables:

- PF/VF-style tenant partition abstraction
- atomic write capability model
- optional offload / in-storage-compute abstraction
- deferred mode personas such as SEF or Open Channel where justified

Exit criteria:

- advanced capabilities remain clearly optional and profile-gated
- no advanced feature is enabled by default without an explicit persona selection

---

## 9. Validation Strategy

### 9.1 Test Categories

- **profile tests**: each persona exposes the expected capabilities
- **scheduler/QoS tests**: latency, bandwidth, and fairness behavior under contention
- **mode tests**: conventional vs ZNS vs FDP-oriented behavior
- **telemetry tests**: queue pressure, maintenance state, and health visibility
- **security lifecycle tests**: secure boot, key state, firmware protection transitions
- **compatibility tests**: existing baseline NVMe/controller behavior remains stable when advanced persona features are off

### 9.2 Must-Have Checks

- baseline profile preserves today's expected controller behavior
- 8ch and 16ch personas produce measurably different envelopes
- dual-port support is persona-gated and testable
- ZNS/FDP behavior is persona-driven rather than hard-coded globally
- telemetry reflects maintenance and queue pressure
- security lifecycle changes controller state in observable ways
- controller timing changes consistently when the underlying NAND/media profile changes

---

## 10. Risks And Mitigations

### Risk 1: Overfitting to marketing collateral

Mitigation:

- use vendor materials only to define public behavior classes
- avoid pretending undocumented internals are known

### Risk 2: Controller design drifts away from NAND/media design

Mitigation:

- make controller profile reference a NAND/media profile explicitly
- keep mode/persona design aligned with the NAND/media capability roadmap

### Risk 3: Feature growth breaks baseline test stability

Mitigation:

- keep a baseline generic profile that preserves current behavior
- gate enterprise behavior behind explicit persona selection

### Risk 4: Too many advanced features arrive at once

Mitigation:

- split enhancement into profile, QoS, mode, lifecycle, and advanced-extension phases
- keep PF/VF, offload, and SEF/Open Channel clearly deferred

---

## 11. Open Decisions Already Resolved

The following decisions are fixed for this design:

- the controller simulator should be organized by **common enterprise capability classes**, not by vendor-specific clone efforts
- Marvell, SM8366, and Tacoma act as **input sources**, not exact emulation targets
- generic controller personas come before vendor-inspired presets
- ZNS and FDP are earlier-priority controller modes than PF/VF or compute offload
- controller/media coupling is a design requirement, not a later optimization

---

## 12. References

- NAND/media design: [`2026-04-10-nand-media-command-coverage-design.md`](2026-04-10-nand-media-command-coverage-design.md)
- Marvell Bravera SC5 product page: <https://www.marvell.com/products/ssd-controllers/mv-ss1331-1333.html>
- Marvell Bravera SC5 product brief: <https://www.marvell.com/content/dam/marvell/en/public-collateral/storage/marvell-ssd-mv-ss1331-1333-product-brief.pdf>
- Silicon Motion enterprise controller page: <https://www.siliconmotion.com/products/enterprise/detail>
- Silicon Motion SM8366 product brief: <https://www.siliconmotion.com/download/3k5/a/SM8366_PB_EN.pdf>
- InnoGrit product page: <https://www.innogritcorp.com/product.html>
- InnoGrit Tacoma Gen5 announcement: <https://www.innogritcorp.com/newsdetail.html?id=43>