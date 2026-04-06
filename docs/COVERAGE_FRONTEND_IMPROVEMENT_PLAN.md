# Front-End Coverage Improvement Plan

## Purpose

This document turns the current UT/E2E coverage reports into a concrete plan
for improving coverage of the guest-visible front-end and its adjacent control
path components.

The intent is not to maximize aggregate coverage percentage blindly. The goal
is to close specific blind spots in the path that starts at guest-visible NVMe
commands and ends in the simulator's front-end queueing and control-plane
logic.

## Current Baseline

Baseline below was captured from the latest `master` at commit `ea76e3c`
using:

```bash
make coverage-clean
make coverage-build
make coverage-ut
bash scripts/coverage/ratchet_check.sh
make coverage-e2e
make coverage-merge
```

### Report Summary

| Report | Lines | Functions | Branches |
|--------|-------|-----------|----------|
| UT | 73.2% | 84.2% | 53.7% |
| E2E | 28.9% | 33.9% | 19.1% |
| Merged | 73.4% | 84.2% | 55.1% |

### Important Interpretation

The front-end gap is not "coverage is low everywhere." The actual issue is
more specific:

- some front-end support code is already covered indirectly
- several control-plane and queue-management modules remain almost entirely
  unexecuted
- some modules are only covered on simple happy paths and miss blocking,
  timeout, or command-processing branches
- the current default coverage reports exclude `src/vhost/*`, which means the
  guest-facing NBD transport path is intentionally invisible in the standard
  coverage dashboard

## Architectural Enhancement: NVMe Command Dispatch Layer

**Status: DONE**

A fundamental gap existed in the E2E coverage pipeline: the NBD server called
`nvme_uspace_read/write/flush/trim()` directly, bypassing the NVMe command
processing functions (`nvme_ctrl_process_io_cmd()`, `nvme_ctrl_process_admin_cmd()`)
in `src/pcie/nvme.c`.  This meant the NVMe command validation and dispatch layer
received zero coverage from E2E tests.

### Changes

1. **New dispatch functions** (`src/pcie/nvme_uspace.c`):
   - `nvme_uspace_dispatch_io_cmd()` — routes I/O through
     `nvme_ctrl_process_io_cmd()` then dispatches to `nvme_uspace_*` backends
   - `nvme_uspace_dispatch_admin_cmd()` — routes admin commands through
     `nvme_ctrl_process_admin_cmd()` then dispatches to identify, get/set
     features, log pages, queue management, format, sanitize, firmware ops
   - `nvme_uspace_exercise_admin_path()` — exercises 13 admin commands through
     the full dispatch path at NBD server startup

2. **NBD server integration** (`src/vhost/hfsss_nbd_server.c`):
   - Single-threaded I/O now builds proper NVMe SQEs and routes through
     `nvme_uspace_dispatch_io_cmd()` instead of calling `nvme_uspace_*` directly
   - Admin command exercise runs at startup, covering identify, features, log
     pages, queue create/delete, keep-alive, and unsupported opcode handling
   - Multi-threaded mode still uses `mt_io()` for TAA shard cache coherency

3. **E2E coverage collection** (`scripts/coverage/run_e2e_coverage.sh`):
   - Now runs blackbox nvme-cli test cases (001–006) after the fio suite
   - Captures coverage from both fio I/O patterns and nvme-cli admin commands

### Coverage Impact

Before: `nvme_ctrl_process_io_cmd()` and `nvme_ctrl_process_admin_cmd()` had
zero E2E coverage.

After: Every single-threaded I/O request from QEMU exercises the full NVMe
command processing pipeline (SQE → opcode validation → uspace dispatch → FTL →
CQE).  Admin commands are exercised at startup.

---

## Gap Analysis

### 1. `msgqueue` is under-tested, but not untested

File:
- `src/common/msgqueue.c`

Current state:
- basic `init/send/recv/try*` behavior is covered from `tests/test_common.c`
- this file is not a true zero-coverage area

Missing coverage:
- bounded blocking behavior
- timeout behavior in `msg_queue_send()` and `msg_queue_recv()`
- producer/consumer wakeup behavior
- wraparound under repeated enqueue/dequeue cycles
- stats-path validation

Impact:
- regressions in queue blocking semantics can survive CI even though basic API
  calls still work

### 2. `shmem_if` is effectively uncovered

File:
- `src/controller/shmem_if.c`

Current state:
- coverage is effectively zero
- no dedicated unit test exists

Missing coverage:
- `shmem_if_open()`
- `shmem_if_close()`
- `shmem_if_receive_cmd()`
- `shmem_if_send_cpl()`
- ring/index wrap behavior
- empty/full boundary handling

Impact:
- this control-plane ingress/egress layer has no real regression protection

### 3. PCIe queue management is only partially exercised

File:
- `src/pcie/queue.c`

Current state:
- basic queue creation/destruction paths are covered
- many completion-side and queue-management paths remain unexecuted

Notable missing or weakly covered areas:
- `nvme_cq_update_head()`
- `nvme_cq_post_cpl()`
- `nvme_cq_needs_interrupt()`
- `nvme_create_io_sq()`
- `nvme_create_io_cq()`
- invalid/duplicate/busy delete paths

Impact:
- queue lifecycle bugs can slip through while the user-space device still
  appears to work in light testing

### 4. NVMe admin/io command processing is still thin

File:
- `src/pcie/nvme.c`

Current state:
- controller init/cleanup is covered
- actual admin/io command processing paths are barely exercised

Weak areas:
- `nvme_ctrl_process_identify()`
- `nvme_ctrl_process_admin_cmd()`
- `nvme_ctrl_process_io_cmd()`
- unsupported opcode handling
- invalid field / malformed command responses

Impact:
- control-plane behavior is still mostly validated indirectly, not directly

### 5. `nvme_uspace` has broad but uneven coverage

File:
- `src/pcie/nvme_uspace.c`

Current state:
- lifecycle, data I/O, trim, flush, identify, and some feature/log-page calls
  are already exercised

Remaining weak areas:
- firmware download / commit path
- sanitize path
- less common feature and log-page branches
- invalid combinations and negative-path validation

Impact:
- the user-space front-end contract is only partially protected outside the
  core read/write/trim flows

### 6. The default coverage reports under-represent the guest-facing front-end

Current exclusion rule in standard reports:
- `src/vhost/*` is removed from UT and E2E coverage

Why this matters:
- guest-visible blackbox testing goes through `hfsss-nbd-server`
- even when blackbox/E2E runs hit real front-end transport behavior, that
  coverage is filtered out of the standard report

Impact:
- front-end transport improvements can happen without becoming visible in the
  default coverage dashboard

## Coverage Improvement Goals

### Goal A

Raise confidence in the front-end control plane, not just data-path I/O.

### Goal B

Eliminate true zero-coverage front-end modules in the default merged report,
starting with `shmem_if`.

### Goal C

Expand front-end branch coverage enough that queue lifecycle, admin command
handling, and blocking semantics are protected by CI.

### Goal D

Add a nightly front-end-inclusive view so transport-layer coverage is visible
without destabilizing the fast PR ratchet.

## Implementation Plan

### Phase 0: Close the Most Critical UT Gaps

Priority: `P0`

### Task 0.1: Add `tests/test_msgqueue.c`

Purpose:
- move `msgqueue` coverage from basic API smoke to behavioral coverage

Required scenarios:
- `msg_queue_send()` timeout on full queue
- `msg_queue_recv()` timeout on empty queue
- `msg_queue_send(..., timeout_ns=0)` busy path
- `msg_queue_recv(..., timeout_ns=0)` no-entry path
- producer/consumer wakeup with two threads
- ring wraparound after repeated send/recv
- `msg_queue_stats()`
- invalid argument checks

Expected impact:
- better branch coverage in `src/common/msgqueue.c`
- direct protection of blocking semantics used by future front-end paths

### Task 0.2: Add `tests/test_shmem_if.c`

Purpose:
- bring `src/controller/shmem_if.c` into real CI coverage

Required scenarios:
- open/create temporary shared-memory backing
- receive command from empty/non-empty ring
- send completion into empty/non-empty ring
- producer/consumer index wraparound
- full-slot / no-space handling
- close/cleanup path

Implementation note:
- use temporary per-test shm names
- keep this test process-local; it does not need QEMU or a kernel component

Expected impact:
- remove a true zero-coverage front-end control module

### Task 0.3: Extend `tests/test_pcie_nvme.c`

Purpose:
- cover completion-side queue behavior in `src/pcie/queue.c`

Required scenarios:
- `nvme_cq_update_head()`
- `nvme_cq_post_cpl()`
- `nvme_cq_needs_interrupt()`
- duplicate create failure
- invalid queue-id / size handling
- delete-busy and delete-clean sequencing

Expected impact:
- stronger queue-manager coverage without requiring guest boot

### Phase 1: Directly Test Admin and IO Command Processing

Priority: `P1`

### Task 1.1: Add `tests/test_nvme_admin_cmds.c`

Purpose:
- cover `src/pcie/nvme.c` admin command handlers directly

Required scenarios:
- Identify Controller
- Identify Namespace
- Get Features
- Set Features
- Get Log Page
- unsupported admin opcode
- invalid field / malformed request handling

Expected impact:
- direct coverage of `nvme_ctrl_process_admin_cmd()`

### Task 1.2: Add `tests/test_nvme_io_cmds.c`

Purpose:
- cover `nvme_ctrl_process_io_cmd()` directly

Required scenarios:
- read
- write
- flush
- DSM / trim
- invalid NSID
- out-of-range LBA
- zero-length / malformed request
- unsupported IO opcode

Expected impact:
- direct front-end IO command coverage independent of guest orchestration

### Task 1.3: Extend `tests/systest_nvme_compliance.c`

Purpose:
- deepen front-end contract testing for `nvme_uspace`

Required additions:
- firmware download / commit mock path
- unsupported log page behavior
- format path coverage
- sanitize-path contract checks
- additional negative-path feature combinations

Expected impact:
- reduce the number of unhit admin/service branches in
  `src/pcie/nvme_uspace.c`

### Phase 2: Increase Guest-Visible Front-End Coverage in Blackbox CI

Priority: `P1`

### Task 2.1: Add PR-smoke `nvme-cli` admin cases

**Status: DONE**

Implemented:
- `005_nvme_get_set_features_smoke.sh` — Get/Set Features (FID 0x07, 0x02, 0x04)
- `006_nvme_smart_log_smoke.sh` — SMART/Health log + Error log

Both cases added to the PR smoke bundle in `blackbox.yml`.

Purpose:
- make the PR smoke path cover more than enumerate/flush plus fio

Constraints:
- remain fast enough for PR gate budget
- avoid destructive admin operations

Expected impact:
- more real guest-visible admin path coverage in the E2E layer

### Task 2.2: Add nightly-only destructive admin cases

Candidates:
- format
- firmware management mock path
- sanitize / unsupported admin behavior

Purpose:
- keep PR smoke lean while giving nightly coverage of riskier front-end flows

### Task 2.3: Add nightly queue-pressure fio case

Suggested shape:
- `4k randrw`
- elevated `iodepth`
- mixed read/write
- verify enabled

Purpose:
- push queue and completion behavior harder than the current smoke cases

Expected impact:
- more realistic front-end branch coverage in queue-related code

### Phase 3: Improve Reporting So Front-End Progress Is Visible

Priority: `P2`

### Task 3.1: Add a nightly front-end-inclusive coverage report

Current limitation:
- standard coverage removes `src/vhost/*`

Plan:
- keep PR ratchet aligned with current exclusions
- add an additional nightly merged report that includes:
  - `src/vhost/hfsss_nbd_server.c`
  - `src/vhost/nbd_async.c`

Purpose:
- expose transport-layer front-end coverage without destabilizing fast CI

### Task 3.2: Publish a module-level coverage delta summary

For each nightly run, summarize:
- `msgqueue`
- `shmem_if`
- `pcie/queue`
- `pcie/nvme`
- `pcie/nvme_uspace`
- `vhost/nbd_async`
- `vhost/hfsss_nbd_server`

Purpose:
- prevent progress from being hidden by stable global percentages

## CI Landing Sequence

Recommended order:

1. `tests/test_shmem_if.c`
2. `tests/test_msgqueue.c`
3. `tests/test_pcie_nvme.c` queue extensions
4. `tests/test_nvme_admin_cmds.c`
5. `tests/test_nvme_io_cmds.c`
6. PR-smoke `nvme-cli` admin cases
7. nightly destructive admin cases
8. nightly front-end-inclusive coverage report

This order prioritizes:
- zero-coverage removal first
- fast UT wins before heavier guest-based work
- reporting improvements only after there is meaningful new front-end signal

## Exit Criteria

Phase 0 is complete when:
- `src/controller/shmem_if.c` is no longer effectively zero coverage
- `src/common/msgqueue.c` covers timeout/wakeup branches, not just basic send/recv
- `src/pcie/queue.c` covers completion update/post paths

Phase 1 is complete when:
- `src/pcie/nvme.c` admin/io handlers are directly exercised in tests
- `src/pcie/nvme_uspace.c` unsupported/admin side branches are materially reduced

Phase 2 is complete when:
- PR smoke includes at least one guest-visible admin coverage case beyond flush
- nightly runs include at least one destructive/extended admin case

Phase 3 is complete when:
- nightly coverage exposes front-end transport modules in a dedicated report
- module-level delta reporting exists for front-end hot spots

## Non-Goals

This plan does not attempt to:
- raise aggregate coverage by adding low-value assertions
- force `src/vhost/*` into the fast PR ratchet immediately
- replace existing blackbox functional goals with coverage-only goals

The objective is targeted front-end risk reduction, not coverage vanity.
