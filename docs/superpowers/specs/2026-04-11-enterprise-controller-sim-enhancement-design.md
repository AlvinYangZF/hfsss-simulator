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
- host-visible namespace-mode differences such as conventional NVMe, ZNS namespaces, and FDP-oriented placement behavior
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
- no first-class distinction between 8-channel and 16-channel enterprise controller profiles, or between single-port and dual-port variants
- no **dual-port** host model
- no storage-centric multi-domain or tenant-partitioning model
- scheduler behavior is materially simpler than public enterprise QoS / arbiter claims
- no namespace-scoped **ZNS / FDP** capability model or deferred mode-extension path for **SEF / Open Channel**
- no controller-level **atomic write** or offload abstraction
- telemetry, QoS, security, and NAND/media timing are present as separate subsystems rather than one coherent enterprise-controller behavior

### 2.3 Related design work already in repo

This design should be read alongside:

- [`2026-04-10-nand-media-command-coverage-design.md`](2026-04-10-nand-media-command-coverage-design.md)
- [`HLD_01_PCIE_NVMe_EMULATION_EN.md`](../../../docs/HLD_01_PCIE_NVMe_EMULATION_EN.md)
- [`LLD_18_QOS_DETERMINISM_EN.md`](../../../docs/LLD_18_QOS_DETERMINISM_EN.md)
- [`LLD_19_SECURITY_ENCRYPTION_EN.md`](../../../docs/LLD_19_SECURITY_ENCRYPTION_EN.md)

The NAND/media design handles command-state and media-side realism. This controller design handles **host-facing policy, persona, and enterprise feature behavior**.

### 2.4 Current-code prerequisites that affect phase ordering

This spec is intentionally ahead of the current master branch. The implementation plan must account for the following already-known prerequisites:

- multi-namespace host dispatch is not active in the current NVMe user-space path, so Phase 2 namespace-mode work depends on either real multi-namespace plumbing or an explicitly bounded prerequisite patch
- a controller lifecycle state machine does not exist on master, so a **minimal lifecycle skeleton** must land before any phase that claims lifecycle-gated command legality, admission control, or telemetry visibility
- the current register model and BAR layout only expose a 64-doorbell baseline, so queue-pair counts advertised by future personas must be treated as profile-envelope inputs until the register model is expanded
- the current `hfsss_config` loader is flat and does not support array-shaped profile data, so the initial profile rollout should use compile-time C tables, with config-file import/export extended later if needed

---

## 3. Public Capability Baseline

### 3.1 Cross-vendor common signals

Across the three public controller families, the following common signals are strong enough to drive a simulator design:

- PCIe **Gen5 x4** is the baseline host link
- high NAND parallelism is normal: **8-channel and 16-channel classes**, with vendor-specific extension beyond that
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
  - strongest inputs: 8/16-channel split, dual-port, multi-domain isolation, SLA/QoS, SEF/ZNS/Open Channel flexibility

- **Silicon Motion SM8366**
  - strongest inputs: 16-channel enterprise profile, 1024 queue pairs, 128 namespaces, PerformaShape, FDP, host-based FTL cooperation

- **InnoGrit Tacoma IG5669**
  - strongest inputs: 16-channel enterprise profile with tight NAND/media coupling, ZNS, atomic write, low-latency persona, optional offload behavior

Published maxima such as `1024 queue pairs` or `128 namespaces` should be treated as **persona-envelope targets**, not as a claim that Phase 1 reaches immediate parity with those numbers on the existing register model.

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
- add host-visible namespace-mode differences such as ZNS and FDP-oriented behavior
- couple controller timing and policy more tightly to NAND/media profile
- add controller telemetry and security lifecycle modeling that is coherent at the persona level
- preserve a compatible baseline path for existing NVMe and controller tests where enterprise features are not enabled

### 4.2 Non-goals

- exact reproduction of Marvell, Silicon Motion, or InnoGrit firmware internals
- vendor-private command-set emulation in the first implementation
- SR-IOV-style virtualization fidelity or computational-storage API fidelity beyond public behavior classes
- cycle-accurate host interface timing
- full cloud-SSD software stack emulation outside the controller boundary
- default OCP Datacenter NVMe SSD conformance in the first implementation
- full NVMe-MI, TCG SSC, or enterprise-admin-command fidelity before the relevant later phases explicitly pull them into scope

---

## 5. Proposed Architecture

The enhanced simulator should split controller behavior into five layers.

### 5.1 Controller Profile Layer

This layer defines the public controller persona.

Responsibilities:

- define stable **orthogonal axes** for controller modeling:
  - topology
  - host-visible capability surface
  - namespace-mode capability matrix
  - policy bundle
  - lifecycle bundle
- define power, latency, and throughput envelopes
- own persona-level command-semantic capability gates such as atomic write, dual-port exposure, multi-domain partitioning, and optional offload
- bind these axes into a concrete shipped profile without collapsing the axes themselves into one monolithic preset

The key rule is: **topology, namespace mode, and policy are separate axes**. A controller profile is a composition of those axes, not a single bag of flags.

Terminology used in this document is fixed as follows:

- `profile`: the structured field model and capability schema
- `persona`: a named instance of that profile model
- `preset`: a compile-time starter configuration used to instantiate a persona during early implementation

### 5.2 Enterprise Isolation Layer

This layer models how the controller separates workloads and controls latency.

Responsibilities:

- queue arbitration and dispatch policy across explicit arbitration domains
- namespace-aware QoS
- latency target enforcement
- multi-domain isolation hooks
- maintenance interference accounting

The initial arbitration domains are:

- admin
- foreground host I/O
- metadata and controller housekeeping
- background maintenance
- port/path management

This layer owns **how** work is scheduled once a capability is legal. It does not decide which namespace modes are legal or which telemetry fields exist.

### 5.3 Mode And Placement Layer

This layer represents controller-visible **namespace** and placement semantics.

Responsibilities:

- conventional namespace semantics
- ZNS namespace semantics
- FDP-oriented placement semantics
- later extension for SEF / Open Channel

This layer deliberately treats ZNS and FDP as **different contracts**:

- ZNS is a namespace-facing command-set and state-machine contract
- FDP is a placement and reclaim-policy contract layered onto the conventional block model

They may coexist under one controller persona, but they should not be modeled as one symmetric feature bucket.

This layer defines the host-visible contract for:

- identify data exposed to the host
- command legality
- completion semantics
- namespace-scoped invariants such as zone state and write-pointer behavior

`atomic write` is **not** owned by this layer. It is a command-semantic capability that affects completion and failure behavior across layers.

### 5.4 Media Coupling Layer

This layer connects controller persona to NAND/media capabilities instead of treating media as interchangeable.

Responsibilities:

- bind controller profile to NAND/media profile
- vary controller timing by NAND interface generation and media class
- express how channel count, ONFI/Toggle generation, and media type affect controller-level performance and latency

This layer owns the contract between controller and NAND/media. It must define:

- timing vectors the controller may consume
- legality/capability feed from media to controller
- suspend/status snapshot surfaces exposed upward from media
- reset-abort and media-error propagation semantics

This layer does **not** override host-visible capability surfaces on its own; it feeds timing and legality into the higher controller layers.

Controller work in Phase 2 and beyond depends on the NAND/media design reaching the corresponding command-state, legality-feed, and status-snapshot milestones. The implementation plan should therefore pin each controller phase to the NAND/media phase it consumes instead of assuming both tracks advance independently.

### 5.5 Enterprise Lifecycle Layer

This layer owns controller state that matters to operations and security.

Responsibilities:

- secure boot and firmware protection state
- key-management and encryption lifecycle
- telemetry and health-state reporting
- maintenance-state visibility
- optional offload / in-storage-compute abstraction in later phases

This layer must be defined as an explicit controller lifecycle and recovery state machine rather than a loose collection of features.

### 5.6 Layer Ownership And Precedence

The layers above are not peers. Conflict resolution must follow a fixed order:

1. **Lifecycle layer**
   - global safety gates
   - boot / failed / recovering / sanitize / crypto-erase / failover states
2. **Mode and placement layer**
   - namespace legality
   - host-visible command and identify semantics
3. **Controller profile layer**
   - topology, scale limits, and capability envelopes
4. **Enterprise isolation layer**
   - arbitration, QoS, and maintenance prioritization within legal work
5. **Media coupling layer**
   - latency, legality refinement, and error propagation from NAND/media

This prevents capability gating, maintenance throttling, and media-derived timing from turning into scattered special cases.

---

## 6. Controller Capability Model

### 6.1 Baseline controller profile fields

The first controller-profile abstraction should at minimum define:

- `profile_version`
- `profile_name`
- `pcie_gen`
- `pcie_lane_width`
- `port_topology`
- `port_count`
- `lanes_per_port`
- `identify_ctrl_surface`
- `identify_ns_surface`
- `command_set_bitmap`
- `feature_bitmap`
- `log_page_bitmap`
- `channel_count`
- `namespace_limit`
- `queue_pair_limit`
- `supports_dual_port`
- `supports_atomic_write`
- `supports_multi_domain_partitioning`
- `supports_offload`
- `namespace_mode_matrix`
- `latency_envelope`
- `bandwidth_envelope`
- `power_envelope`
- `telemetry_families`
- `telemetry_sampling_contract`
- `security_lifecycle_class`
- `nand_media_profile_ref`
- `maintenance_service_class`

In Phase 1, some of these fields are expected to exist as **declared profile metadata** before their full runtime semantics land. In particular, `supports_dual_port`, `supports_atomic_write`, `supports_multi_domain_partitioning`, `supports_offload`, and `security_lifecycle_class` may initially be authoritative profile slots whose enforcing behavior arrives in later phases.

### 6.2 Initial persona set

The first wave should define generic personas rather than vendor-branded replicas:

- `generic_gen5_enterprise_8ch_single_port`
- `generic_gen5_enterprise_16ch_single_port`
- `generic_gen5_enterprise_16ch_dual_port`

Vendor-inspired presets can then map onto these generic personas:

- Marvell-inspired profile family
- SM8366-inspired profile family
- Tacoma-inspired profile family

Namespace-mode capability is layered on top of those personas through `namespace_mode_matrix`, not by creating device-wide `zns` or `fdp` controller identities.

### 6.3 Why generic personas come first

The public data is sufficient to justify **behavior classes**, but not sufficient to justify faithful vendor replication. Generic personas avoid fake precision while still letting HFSSS simulate meaningful enterprise-controller differences.

### 6.4 Port And Path Model

Dual-port must be modeled as a path-state system, not just as port contention.

The minimum path states are:

- `PATH_ACTIVE_OPTIMIZED`
- `PATH_ACTIVE_NONOPTIMIZED`
- `PATH_STANDBY`
- `PATH_FAILING_OVER`
- `PATH_FAILED`

Path state is host-visible through telemetry and affects queue admission, ownership, and failover behavior.

The minimum path transitions that must be explicit are:

- optimized path degraded to non-optimized after ownership change or controlled failover
- active path to failing-over when path loss or controller-side fault is detected
- failing-over to active/non-optimized or failed depending on recovery outcome
- standby to active/non-optimized when takeover succeeds

The first implementation does not need ANA-perfect emulation, but it must preserve deterministic path-state transitions and machine-readable readback.

### 6.5 Namespace Mode Matrix

The namespace-mode contract is namespace-scoped, not controller-wide.

The first implementation should support a matrix such as:

- conventional namespaces allowed
- ZNS namespaces allowed
- FDP-enabled namespaces allowed
- coexistence rules between those namespace classes

This allows one controller persona to expose mixed conventional and ZNS/FDP namespaces where the active capability set permits it.

### 6.6 NVMe Hierarchy Assumptions

The controller simulator should keep the NVMe hierarchy explicit even before every layer is fully implemented.

The working ownership model is:

- **controller/path scope**: host-path visibility, path state, failover, and controller-wide safety gates
- **endurance-group scope**: endurance accounting, reclaim-policy grouping, and future FDP-aligned placement domains
- **NVM-set scope**: optional media/performance grouping, deferred unless a later phase needs it explicitly
- **namespace scope**: host-visible namespace-mode semantics such as conventional NVM or ZNS behavior

The first implementation does not need complete NVMe hierarchy fidelity, but it should avoid collapsing every future enterprise feature into namespace scope by default.

### 6.7 Minimum Host-Visible Command Scope

The controller profile fields in this spec imply a minimum host-visible command-scope table. The implementation plan should freeze this table before code starts.

Baseline admin-scope surface:

- Identify Controller / Namespace / active namespace enumeration
- Get Log Page for health, error, telemetry, and lifecycle surfaces explicitly exposed by the active profile
- Get/Set Features only for feature scopes surfaced by the active profile and safe under the current lifecycle state

Baseline I/O-scope surface:

- NVM Command Set `Read`, `Write`, `Flush`, `Write Zeroes`, and Dataset Management / Trim

Deferred or profile-gated admin surface:

- Namespace Management create/delete/attach/detach until multi-namespace plumbing is real
- Firmware Download / Commit, Sanitize, Security Send / Receive, Virtualization-related admin flows, and NVMe-MI-aligned management hooks until the relevant phases pull them into scope

Deferred or profile-gated I/O surface:

- Compare, Verify, Copy, Reservations, and other advanced NVM Command Set behaviors
- ZNS-specific commands such as Zone Append and Zone Management Send / Receive until the ZNS namespace contract phase
- FDP-related directives, identifiers, and logs until the FDP-oriented placement phase

### 6.8 Controller-To-FTL And Application Contract

The controller simulator must define what it owns versus what the FTL/application layers own.

Controller-owned contracts:

- identify and capability exposure
- host-visible command legality
- completion status and ordering guarantees
- atomic-write completion/failure semantics
- namespace-mode invariants visible to the host

FTL/application-owned contracts:

- physical placement algorithms
- persistent zone bookkeeping implementation
- placement-policy realization behind FDP-like hints
- metadata persistence and recovery details

Shared contract:

- zone-state transitions
- write-pointer updates
- reset-abort outcome propagation
- media error to completion mapping
- placement-hint to media execution bridge

This keeps ZNS/FDP from becoming decorative labels or from swallowing FTL responsibility into controller code.

### 6.9 Controller Lifecycle And Recovery State Model

The lifecycle layer must be represented as an explicit state machine with named states and externally visible transitions.

The minimum controller lifecycle states are:

- `CTRL_BOOTSTRAPPING`
- `CTRL_READY`
- `CTRL_DEGRADED`
- `CTRL_FAILING_OVER`
- `CTRL_RECOVERING`
- `CTRL_SANITIZING`
- `CTRL_LOCKED_DOWN`
- `CTRL_FAILED`

The first implementation must define deterministic entry conditions and externally visible effects for at least:

- cold boot to ready
- firmware authentication failure to locked-down or failed
- path or controller fault to degraded or failing-over
- recovery completion back to ready or degraded
- sanitize or crypto-erase entry and completion
- fatal unrecoverable fault to failed

This state machine owns controller-wide safety gates. Queue admission, maintenance execution, telemetry visibility, and security-sensitive command legality must all respect the active lifecycle state.

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

Dual-port is public for Marvell and SM8366. Tenant isolation is strongly implied by multi-function and namespace/QoS claims.

The simulator should therefore support:

- optional dual-port host persona
- queue ownership and contention between host paths
- namespace and controller-resource partitioning
- later multi-domain or multi-function partition abstraction

The terminology should stay storage-centric. This design should avoid importing a NIC-style PF/VF mental model unless later public evidence requires it.

### 7.3 Mode And Placement Personas

Namespace-mode differences should be explicit, but they should remain namespace-scoped rather than controller-wide.

Required early behaviors:

- conventional namespace mode
- ZNS namespace support
- FDP-oriented placement support

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

The telemetry surface should be broken into explicit families:

- health
- error
- endurance and wear
- thermal and power
- maintenance
- port/path
- QoS and latency

### 7.6 Maintenance Service Model

Maintenance must be a first-class controller service class, not just an interference note.

The first named maintenance operations are:

- garbage collection
- wear leveling
- media scan / refresh
- rebuild or parity recovery
- metadata scrub or checkpoint work

For each service class, the design must define:

- whether it is preemptible by host I/O
- which arbitration domain it belongs to
- which telemetry counters it updates
- whether it may be paused or degraded during failover or security transitions

### 7.7 External Observability Contract

Every state introduced by this design must have a machine-readable readback path.

The minimum external observability contract must expose:

- active controller profile id and version
- namespace-mode matrix currently enabled
- path state per host path
- queue depth and dispatch counters
- SLA violation counters and latency histogram window
- maintenance service states
- secure boot and firmware-protection state
- key lifecycle state
- active NAND/media profile reference

The first implementation may expose this through an OOB or debug API. Later phases may map parts of it onto NVMe telemetry and log pages, but the observability contract must exist from the beginning.

OCP-aligned log-page fidelity is a later enhancement, not an automatic Phase 1-3 promise. The early requirement is schema stability and machine-readable observability, not default conformance to every OCP log-page contract.

---

## 8. Phase Plan

### Phase 0: Capability Matrix And Persona Freeze

Deliverables:

- controller capability matrix derived from the three research documents
- generic enterprise persona definitions
- mapping from vendor-inspired inputs to generic personas
- capability-to-observable-to-test traceability matrix
- axis ownership matrix covering topology, mode, policy, lifecycle, and media coupling
- current-code prerequisite matrix covering multi-namespace plumbing, lifecycle skeleton needs, queue-scale register implications, and profile-representation constraints

Exit criteria:

- every proposed capability is marked as:
  - baseline
  - optional
  - deferred
- each initial persona has a stable field set

### Phase 1: Controller Profile Foundation

Deliverables:

- controller profile abstraction
- generic 8ch / 16ch persona presets
- compile-time profile table bootstrap for the initial personas
- controller configuration path updated to consume a profile instead of only flat defaults
- compatibility wrapper path for existing controller startup
- compatibility and migration matrix from current flat configs to the profile model

Exit criteria:

- current default behavior remains available through a baseline profile
- channel count, queue scale, and enterprise capability flags come from the active profile
- fields whose semantics land in later phases are explicitly marked as metadata-only until their enforcing phase begins
- existing controller and NVMe tests still pass in baseline mode

### Phase 2: Namespace Mode And Placement Contract

Deliverables:

- minimal controller lifecycle skeleton sufficient for command-legality, admission, and telemetry-visibility gates used by Phase 2 and Phase 3
- multi-namespace dispatch foundation or explicitly integrated prerequisite path for namespace-scoped behavior
- conventional namespace contract
- ZNS namespace contract
- FDP-oriented placement contract
- controller-to-FTL/application contract for namespace-mode behavior
- initial external observability contract

Exit criteria:

- lifecycle gates required by Phase 2 and Phase 3 exist in a minimal but machine-readable form
- namespace-mode legality is explicit and machine-testable
- ZNS/FDP remain namespace-scoped and can coexist with conventional namespaces where the profile allows it
- baseline personas reject unsupported namespace modes deterministically

### Phase 3: Enterprise QoS, Telemetry, And Maintenance Arbitration

Deliverables:

- stronger namespace-aware scheduling behavior
- persona-aware latency/QoS policy layer
- telemetry families and sampling contract
- maintenance service model and arbitration rules
- minimal path-state schema and telemetry stub sufficient for the observability contract

Exit criteria:

- mixed-load behavior differs across controller personas within declared envelope tolerances
- telemetry reflects queue pressure, latency, maintenance state, and path state using the published schema
- Phase 3 scheduling behavior is explicitly defined for conventional-mode semantics plus the namespace-mode rules frozen in Phase 2

### Phase 4: Dual-Port And Enterprise Lifecycle

Deliverables:

- dual-port host model
- path-state model
- enterprise security lifecycle state machine
- firmware protection and key-lifecycle state

Exit criteria:

- dual-port contention and failover behavior are testable
- lifecycle transitions are machine-readable and tied to explicit queue-admission or command-legality effects
- security state affects controller behavior in deterministic ways

### Phase 5: Advanced Enterprise Extensions

Deliverables:

- multi-domain or multi-function tenant partition abstraction
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
- **command-scope tests**: baseline, optional, and deferred admin/I/O surfaces are exposed or rejected exactly as the command-scope table declares
- **scheduler/QoS tests**: latency, bandwidth, and fairness behavior under contention
- **mode tests**: conventional vs ZNS vs FDP-oriented behavior
- **telemetry tests**: queue pressure, maintenance state, and health visibility
- **security lifecycle tests**: secure boot, key state, firmware protection transitions
- **compatibility tests**: existing baseline NVMe/controller behavior remains stable when advanced persona features are off
- **negative and fault-injection tests**: unsupported namespace modes, failover, reset-abort, rollback, power-cycle, and profile mismatch behavior

### 9.2 Must-Have Checks

- baseline profile preserves today's expected controller behavior
- 8ch and 16ch personas produce envelope differences under a fixed workload, fixed seed, fixed warm-up, and declared tolerance band
- dual-port support is persona-gated and testable
- ZNS/FDP behavior is namespace-scoped and profile-gated rather than hard-coded globally
- command/admin scope exposure follows the published scope table, and unsupported commands fail deterministically
- telemetry reflects maintenance and queue pressure
- security lifecycle changes controller state in observable ways
- controller timing changes consistently when the underlying NAND/media profile changes
- unsupported namespace-mode or path-state operations fail with deterministic status and observable counters
- path failover, key rotation, and reset-abort transitions have explicit observable outcomes

### 9.3 Measurable Acceptance Contract

The terms `deterministic`, `observable`, and `different envelope` must map to explicit pass/fail rules.

Each testable persona feature must define:

- workload shape
- warm-up duration
- sampling window
- fixed seed or deterministic stimulus
- primary metric
- comparison baseline
- pass/fail tolerance

Examples:

- throughput or queue-pressure envelope comparisons use a fixed workload and assert against the profile's declared envelope within a configured tolerance band
- latency behavior uses fixed histogram windows and named percentiles such as P50/P99/P99.9
- failover and lifecycle tests assert on explicit state transitions and externally readable counters, not just command success

### 9.4 Observability And Telemetry Schema Contract

The validation plan depends on a stable machine-readable schema.

The first schema must include:

- `profile_id`
- `profile_version`
- `namespace_mode_matrix`
- `path_id`
- `path_state`
- `queue_depth_current`
- `queue_depth_peak`
- `sla_violation_count`
- `maintenance_state`
- `maintenance_op`
- `latency_histogram_window`
- `secure_boot_state`
- `firmware_state`
- `key_lifecycle_state`
- `nand_media_profile_ref`

Tests should assert against these named fields rather than ad hoc implementation details.

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
- keep multi-domain partitioning, offload, and SEF/Open Channel clearly deferred

---

## 11. Open Decisions Already Resolved

The following decisions are fixed for this design:

- the controller simulator should be organized by **common enterprise capability classes**, not by vendor-specific clone efforts
- Marvell, SM8366, and Tacoma act as **input sources**, not exact emulation targets
- generic controller personas come before vendor-inspired presets
- ZNS and FDP are earlier-priority namespace capabilities than multi-domain partitioning or compute offload
- controller/media coupling is a design requirement, not a later optimization

---

## 12. References

### 12.1 Normative and baseline references

- NAND/media design: [`2026-04-10-nand-media-command-coverage-design.md`](2026-04-10-nand-media-command-coverage-design.md)
- NVM Express Base Specification, Revision 2.3: <https://nvmexpress.org/specification/nvm-express-base-specification/>
- NVM Command Set Specification, Revision 1.2: <https://nvmexpress.org/specification/nvm-command-set-specification/>
- NVMe Zoned Namespace Command Set Specification, Revision 1.4: <https://nvmexpress.org/specification/nvme-zoned-namespaces-zns-command-set-specification/>
- NVM Express Management Interface Specification, Revision 2.1: <https://nvmexpress.org/specification/nvme-mi-specification/>
- OCP Datacenter NVMe SSD Specification, Version 2.6: <https://www.opencompute.org/documents/datacenter-nvme-ssd-specification-v2-6-2-pdf>
- PCI Express Base Specification, Revision 5.0 Version 1.0: <https://pcisig.com/PCIExpress/Specs/Base/_5.0_1.0>
- ONFI 6.0 and ONFI 5.2 specifications: <https://onfi.org/specs.html>
- TCG Storage Architecture Core Specification, Version 2.01 Revision 1.00: <https://trustedcomputinggroup.org/resource/tcg-storage-architecture-core-specification/>
- TCG Storage Interface Interactions Specification (SIIS), Version 1.20: <https://trustedcomputinggroup.org/resource/storage-work-group-storage-interface-interactions-specification/>
- TCG Storage Security Subsystem Class: Opal, Version 2.30: <https://trustedcomputinggroup.org/resource/storage-work-group-storage-security-subsystem-class-opal/>
- TCG Storage Security Subsystem Class: Enterprise, Version 1.01 Revision 1.00: <https://trustedcomputinggroup.org/resource/storage-work-group-storage-security-subsystem-class-enterprise-specification/>

These references define the semantic baseline for the implementation plan. Moving to newer revisions later should be an explicit spec update, not an accidental drift during coding.

### 12.2 Public controller input sources

- Marvell Bravera SC5 product page: <https://www.marvell.com/products/ssd-controllers/mv-ss1331-1333.html>
- Marvell Bravera SC5 product brief: <https://www.marvell.com/content/dam/marvell/en/public-collateral/storage/marvell-ssd-mv-ss1331-1333-product-brief.pdf>
- Silicon Motion enterprise controller page: <https://www.siliconmotion.com/products/enterprise/detail>
- Silicon Motion SM8366 product brief: <https://www.siliconmotion.com/download/3k5/a/SM8366_PB_EN.pdf>
- InnoGrit product page: <https://www.innogritcorp.com/product.html>
- InnoGrit Tacoma Gen5 announcement: <https://www.innogritcorp.com/newsdetail.html?id=43>
