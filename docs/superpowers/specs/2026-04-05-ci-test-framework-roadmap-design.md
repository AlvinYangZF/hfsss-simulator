# CI Test Framework Roadmap — Design Spec

**Date:** 2026-04-05
**Status:** Draft rev 2 — addressing review feedback
**Scope:** Strategic — defines the end-state CI quality-monitoring system for HFSSS and the incremental path to get there

---

## 1. Motivation

HFSSS has accumulated ~46 test files and 5 categories of Make targets, but **CI only runs compilation checks and UT coverage ratchet**. System tests, stress tests, end-to-end QEMU tests, performance regression detection, memory-safety sanitizers, and static analysis are all manual or non-existent. The codebase is growing (4 merged PRs touching FTL hot paths in the last 2 weeks) and without automated quality gates, regressions will leak to master.

This spec defines:
1. A **five-pillar quality model** that names every dimension CI must protect.
2. A **four-sprint roadmap** to build the missing infrastructure incrementally.
3. A **test taxonomy** that tells contributors *which* tests to run for *which* change.
4. **Concrete success criteria** per sprint so we know when each pillar is "done".

**Why now:** PR #38 (coverage ratchet) and PR #39 (QEMU blackbox framework) are adjacent building blocks that ship independently but compose into something bigger. Without a unifying design, they drift apart and new pillars (perf regression, sanitizers) get bolted on inconsistently.

---

## 2. Baseline Snapshot

> **This section is a time-stamped snapshot**, not a live inventory. It anchors the motivation at the time of writing (commit `b41a663`, 2026-04-05) and will go stale as adjacent PRs land. Future readers should run `ls .github/workflows/` and `grep "^test:\\|^systest:\\|^coverage" Makefile` locally for current state; use this section only to understand the delta the roadmap was designed to close.

**At commit `b41a663` the project had two CI workflows** (`build.yml`, `coverage.yml`) exercising compilation and UT coverage ratchet, and **five testing dimensions with no CI execution**: `make test` (UT binaries), `make systest`, `make stress-long`, `make coverage-e2e`, and the pending `make qemu-blackbox-ci` (PR #39). Performance regression, sanitizer builds, static analysis, and code-format enforcement all lived outside CI entirely. Two independent E2E harnesses (`run_e2e_coverage.sh` and blackbox `env.sh`) boot QEMU+NBD+fio on separate code paths.

The sprints in Section 4 are designed to close these specific gaps; gap descriptions will not be kept current as the roadmap executes.

---

## 3. Five-Pillar Quality Framework

A complete CI quality system for an SSD firmware simulator needs these five orthogonal concerns, each with its own ratchet mechanism:

### Pillar 1 — **Unit Correctness**
- **What:** Every source-level contract (FTL mapping, block allocator, NVMe command parser, etc.) exercised by fast in-process tests.
- **Tool:** `make test`, ~37 binaries.
- **Ratchet:** 100 % pass (block PR on any unit test failure).
- **CI cost:** ~30 s per run.

### Pillar 2 — **System Correctness (in-process)**
- **What:** Multi-module integration inside a single process — data integrity at 95 % utilization, NVMe admin compliance, error boundary behavior.
- **Tool:** `make systest`, 3 binaries covering 616 assertions.
- **Ratchet:** 100 % pass.
- **CI cost:** ~2 min.

### Pillar 3 — **System Correctness (E2E, guest OS)**
- **What:** Real Linux kernel sending NVMe commands over a real transport (NBD/vhost) to the simulator — validates the glue layer the in-process tests can't reach.
- **Tool:** PR #39 blackbox framework (`make qemu-blackbox-ci`).
- **Ratchet:** all cases PASS on PR; any FAIL blocks merge.
- **CI cost:** 8–15 min (smoke subset) / 30–60 min (full matrix, nightly only).

### Pillar 4 — **Observability — Coverage**
- **What:** Quantified evidence that the test pyramid actually exercises the code.
- **Tool:** PR #38 coverage infra (`make coverage-ut`, `make coverage-e2e`, `make coverage-merge`).
- **Ratchet:** line / function / branch ≥ baseline − 2 pp; new-code coverage ≥ 80 %.
- **CI cost:** 2–3 min (UT-only, per PR) / 15–20 min (merged, nightly).

### Pillar 5 — **Defensive Quality (safety + performance)**
Three sub-pillars that each need their own CI job:

| Sub-pillar | Tool | Ratchet |
|-----------|------|---------|
| **5a. Memory safety** | ASan + UBSan build variant, run against `make test` and `make qemu-blackbox-ci` | 0 tolerance — any ASan/UBSan report blocks PR |
| **5b. Concurrency safety** | TSan build variant, run against MT FTL worker paths | 0 tolerance — any TSan report blocks PR |
| **5c. Performance regression** | Parse fio JSON from blackbox → compare to baseline | regression > 20 % blocks PR; 10–20 % warns |

**CI cost estimate:** ASan ~5 min (slower than normal), TSan ~8 min, perf check ~1 min (pure parsing).

---

## 4. Four-Sprint Implementation Roadmap

Each sprint is a set of independent PRs delivering one vertical capability. Sprints have no hard deadline; they define ordering when capacity allows.

### Sprint 1 — Wire the existing assets into CI

**Goal:** Make every already-written test actually run somewhere in CI, with an explicit split between fast per-PR gates and slower nightly observation.

**Trigger discipline (important):**

| Trigger | Scope | Budget |
|---------|-------|--------|
| **Per-PR (`pull_request`)** | `make test`, `make systest`, **blackbox smoke subset only** (nvme-cli cases + minimal fio smoke) | ≤ 15 min total added CI time |
| **Nightly / master-push** | Full blackbox matrix (all cases × all NBD modes), `make stress-long`, soak runs | Up to 60+ min; not in PR critical path |
| **Manual `workflow_dispatch`** | Any subset, for on-demand debugging | N/A |

The full blackbox matrix and soak are **deliberately excluded from per-PR CI** — their cost and flakiness would become the dominant source of CI friction before the signal is mature.

| PR | Deliverable | Prerequisite |
|----|-------------|--------------|
| S1-P1 | Add `make test` step to `.github/workflows/build.yml` (UT execution, not just build) | none |
| S1-P2 | Add `.github/workflows/systest.yml` running `make systest` on ubuntu-latest | S1-P1 |
| S1-P3 | Complete PR #39 + add `.github/workflows/blackbox.yml` with **two jobs**: `blackbox-smoke` (per-PR, nvme-cli subset only) and `blackbox-full` (schedule-triggered, all cases × all modes) | PR #39 merged |
| S1-P4 | Per-case timeout + namespace reset in blackbox `run.sh` | PR #39 merged |
| S1-P5 | Guest image publishing pipeline (GitHub Release + download step) | S1-P3 |
| S1-P6 | QEMU architecture abstraction (`HFSSS_QEMU_ARCH` / `HFSSS_QEMU_ACCEL`) for Linux CI runners | S1-P3 |

**Sprint 1 success criteria:**
- Every PR to master triggers `make test`, `make systest`, and blackbox **smoke subset** in CI.
- All three must pass to merge.
- Full blackbox matrix runs on a nightly schedule (not per-PR).
- Total added per-PR CI time: ≤ 15 min.

### Sprint 2 — Performance regression (two-phase rollout)

**Goal:** Prevent silent IOPS / latency regressions, introduced carefully so the signal is trusted before it becomes a gate.

**Why two phases:** a brand-new perf harness has unknown noise floor, guest-side variance, and baseline calibration challenges. Turning on a hard PR gate before that is measured would produce unexplained red builds and train contributors to ignore the signal.

**Phase 2a — Observation (nightly + warn-only PR comment)**

| PR | Deliverable |
|----|-------------|
| S2a-P1 | `scripts/perf/parse_fio_json.sh` — extract iops / lat_mean / p99 from blackbox fio output |
| S2a-P2 | Nightly workflow (`.github/workflows/nightly.yml`) runs full blackbox matrix and writes daily `perf-trend.json` |
| S2a-P3 | `.perf-baseline.json` seeded from master after ≥ 2 weeks of nightly data so noise floor is known |
| S2a-P4 | `post_perf_summary.sh` — PR comment with perf delta, **warn-only; never blocks merge** |
| S2a-P5 | Document measured noise floor (p95 run-to-run variance) in `docs/PERF_BASELINE.md` |

**Phase 2a success criteria:**
- Nightly perf numbers are published for ≥ 4 weeks.
- PR comments show perf delta but do not fail CI.
- Noise floor documented; thresholds calibrated empirically, not guessed.

**Phase 2b — Gate (promote after calibration)**

| PR | Deliverable |
|----|-------------|
| S2b-P1 | `scripts/perf/ratchet_check.sh` — compare parsed metrics against `.perf-baseline.json`, thresholds derived from measured noise floor (typically 2 × noise) |
| S2b-P2 | Extend `.github/workflows/blackbox.yml` with perf-ratchet step — blocks merge on regressions beyond threshold |
| S2b-P3 | Exemption mechanism (see Section 6.5) for intentional perf trade-offs |

**Phase 2b success criteria:**
- Per-PR perf regression beyond calibrated threshold blocks merge.
- Contributors can override with a documented exemption (Section 6.5).
- Promotion from Phase 2a to 2b requires ≥ 4 weeks of stable observation and explicit reviewer sign-off.

### Sprint 3 — Couple coverage and blackbox

**Goal:** Converge on a **single shared QEMU lifecycle** for E2E testing so coverage and blackbox stop maintaining parallel harness code. The specific interface shape (whether that's a flag, an env var, a new entry-point script, or refactoring one to consume the other) is left to the implementation PRs — this sprint specifies the *shared lifecycle* as the outcome, not the syntax.

The lifecycle concerns being unified: port allocation, workspace isolation (PR #42), QEMU launch parameters, guest SSH readiness detection, graceful shutdown ordering (kill QEMU before NBD to flush `.gcda`).

| PR | Deliverable (shape TBD by implementer) |
|----|-------------|
| S3-P1 | Shared QEMU+NBD lifecycle library consumed by both `run_e2e_coverage.sh` and blackbox `env.sh`. Builds on PR #42's `scripts/lib/run_isolation.sh` |
| S3-P2 | One of: migrate `run_e2e_coverage.sh` to call into the blackbox framework, or extract a common lifecycle layer both scripts consume, or extend blackbox runner to optionally emit coverage data |
| S3-P3 | New-code coverage ratchet — parse `git diff` + tool like `diff_cover` or equivalent against the UT `.info` |
| S3-P4 | Merge coverage + blackbox PR comments into one unified quality comment |
| S3-P5 | Nightly merged coverage report (UT + E2E) uploaded as CI artifact |

**Sprint 3 success criteria:**
- Only **one** place in the repo launches QEMU+NBD for automated tests. Changes to QEMU flags / guest lifecycle / port handling touch one file, not two.
- PR new code line coverage target: ≥ 80 % (with exemption path per Section 6.5 — strictly required thresholds are documented as aspirational targets, not absolute gates, until coverage infra has matured).
- Single PR comment shows: UT coverage %, new-code coverage %, blackbox pass/fail, perf delta.

### Sprint 4 — Defensive quality pillar

**Goal:** Catch memory-safety, concurrency, and code-quality bugs before merge.

| PR | Deliverable |
|----|-------------|
| S4-P1 | `ASAN=1` Makefile variant (`-fsanitize=address -fno-omit-frame-pointer`) |
| S4-P2 | `.github/workflows/asan.yml` — builds with ASAN=1, runs `make test` + `make systest` |
| S4-P3 | `TSAN=1` variant + workflow, focused on MT FTL (`make test_taa test_mt_ftl test_gc_mt test_inflight_pool`) |
| S4-P4 | `UBSAN=1` variant + workflow |
| S4-P5 | `.github/workflows/static-analysis.yml` — cppcheck + clang-tidy |
| S4-P6 | `clang-format` check + pre-commit hook |

**Sprint 4 success criteria:**
- Every PR runs ASan + UBSan on the UT suite.
- TSan runs on the MT-specific test subset.
- Zero cppcheck errors allowed; zero clang-tidy warnings of a selected ruleset.
- `clang-format --dry-run -Werror` passes on all changed files.

---

## 5. Test Taxonomy — Which Tests for Which Change

Contributors need a decision tree. This table goes in a new `docs/TESTING_STRATEGY.md` during Sprint 1.

| Change type | Required local commands | Required CI pass |
|------------|------------------------|-------------------|
| Fix typo / doc change | (none) | build + lint |
| Internal helper (one .c file) | `make test` | build + test + coverage-ut ratchet |
| FTL mapping / allocator / GC change | `make test && make systest && make coverage-ut` | all of Pillar 1–4 |
| HAL / media simulation change | `make test && make systest` | all of Pillar 1–4 |
| NVMe controller command handling | `make test && make systest && make qemu-blackbox` | all of Pillar 1–4, blackbox mandatory |
| `src/vhost/*` change (NBD / vhost-user transport) | `make qemu-blackbox` | blackbox mandatory |
| Multi-threaded / concurrency change | `make test && test_taa test_mt_ftl test_gc_mt` + TSan build | Pillar 1 + 5b |
| Hot I/O path change (perf-sensitive) | `make qemu-blackbox` + fio perf check | Pillar 3 + 5c |

---

## 6. Artifact & Reporting Strategy

All CI jobs MUST produce standardized artifacts for audit and debugging:

| Artifact | Producer | Consumer | Retention |
|----------|----------|----------|-----------|
| `summary.json` | Every CI job | Trend dashboard | 90 days |
| `junit.xml` | test / systest / blackbox | GitHub test report UI | 30 days |
| `coverage-ut/*` | coverage.yml | Developer download | 14 days |
| `blackbox/*` (per-case stdout/stderr + fio JSON) | blackbox.yml | Failure triage | 30 days |
| `.coverage-baseline.json` | ratchet_check.sh | Committed to git | forever |
| `.perf-baseline.json` | perf ratchet | Committed to git | forever |
| `asan.log` / `tsan.log` | sanitizer workflows | Failure triage | 30 days |

**PR-level feedback consolidation:** by end of Sprint 3, one GitHub-bot comment per PR (marker-gated, idempotent) summarizing:
- UT coverage % and delta
- New-code coverage %
- Blackbox smoke PASS/FAIL count
- Performance delta vs baseline
- ASan / TSan / UBSan verdicts
- Links to all artifacts

---

## 6.5 Exemption Paths — Escape Hatches for Gated CI

Universal thresholds are not practical for every PR. Dead-code removal legitimately drops coverage; adding a safety check legitimately costs IOPS; a refactor may trip a cppcheck false positive; TSan may flag a benign race in a scheduler loop. Every gate in this roadmap **must** ship with a documented exemption path, or contributors will either work around it by disabling CI entirely or treat red builds as normal.

### Exemption mechanism

**Per-PR exemption:** add a signed-off commit trailer on the PR's commit(s), recognized by the corresponding ratchet script:

| Trailer | Scope | Who can grant |
|---------|-------|--------------|
| `CI-Exempt-Coverage: <reason>` | Skip UT coverage ratchet and new-code coverage gate | PR author + 1 reviewer sign-off in PR body |
| `CI-Exempt-Perf: <reason>` | Skip perf regression gate | PR author + 1 reviewer sign-off |
| `CI-Exempt-Sanitizer: <asan\|tsan\|ubsan> <reason>` | Skip one specific sanitizer check | PR author + 1 reviewer sign-off |
| `CI-Exempt-Static: <cppcheck\|clang-tidy> <reason>` | Skip one static analyzer | PR author + 1 reviewer sign-off |

**Format example:**
```
CI-Exempt-Coverage: removing 340 lines of dead GC debug code
Reviewed-by: <reviewer>
```

### Ratchet-script responsibility

Each ratchet script (`scripts/coverage/ratchet_check.sh`, `scripts/perf/ratchet_check.sh`, sanitizer wrappers) **must**:

1. Parse the PR's commit trailers when running under `pull_request` event.
2. If matching exemption present, skip the threshold check but **still emit the measured delta** to the PR comment for auditability.
3. Log the exemption in the workflow summary with the reason text.

### What exemptions do NOT skip

- The underlying test still runs and produces artifacts — exemption only skips the *gate decision*.
- Functional correctness gates (unit / systest / blackbox pass/fail) are **never exempt** — they are not ratchets, they are pass/fail correctness checks.

### Review-time discipline

When approving an exempt PR, reviewers should verify the exemption reason is substantiated in the PR description. Abuse (e.g., "CI-Exempt-Coverage: flaky" with no investigation) is a reviewer responsibility to catch, not a tooling responsibility.

---

## 7. Open Questions and Risks

| # | Item | Resolution Plan |
|---|------|----------------|
| 1 | CI runner cost — Sprint 1 adds ~15 min/PR, Sprint 4 adds another ~15 min | Monitor GitHub Actions minute usage; consider self-hosted runners for blackbox (Apple Silicon Mac Studio already used locally) |
| 2 | Guest image distribution (Release vs on-the-fly build) | Sprint 1 PR S1-P5 decides — expect Release is simpler |
| 3 | `.perf-baseline.json` update workflow (same problem as coverage baseline) | Reuse manual `--update-baseline` gate from coverage ratchet |
| 4 | TSan on MT FTL may produce false positives from HAL scheduler | Sprint 4 PR S4-P3 adds `-fsanitize-ignorelist=scripts/tsan.ignore` as escape hatch |
| 5 | Nightly workflow needs a push gate to avoid baseline drift on red master | Gate perf / coverage baseline auto-update behind `[ci skip baseline]` commit trail |
| 6 | QEMU Linux CI runners lack HVF; need KVM enablement | GitHub `ubuntu-latest` does NOT have nested virt; investigate `larger` runners or use docker-based qemu-tcg (slow) |

---

## 8. Out of Scope (explicit non-goals)

- Fuzzing (`libFuzzer` / `AFL++`) — deferred to post-Sprint 4
- Chaos engineering / fault injection at CI level — `test_fault_inject` exists locally, no CI plan yet
- Formal verification of FTL data structures
- Long-duration endurance testing > 24 h (out of CI budget)
- PR benchmarking dashboard with historical graphs (Sprint 3 produces data; a dashboard is a follow-on project)
- Test-selection-by-affected-files (running only impacted tests) — too little CI pain to justify the infrastructure yet

---

## 9. Success Definition

This design is successful when, by end of Sprint 4:

1. **Every PR** merged to master has passed: unit tests, systests, blackbox smoke, UT coverage ratchet, new-code coverage ratchet, ASan, UBSan, (TSan on concurrency changes), cppcheck, clang-format, and perf regression check.
2. **Nightly master** produces: full coverage merged report, soak-level blackbox results, perf baseline reconciliation.
3. **Contributors** can look up `docs/TESTING_STRATEGY.md` and know in < 30 seconds which commands to run for their change.
4. **Incident response time** for a regression on master drops below 24 h because per-PR gates catch 95 % of them before merge.

---

## 10. References

- PR #38 — Firmware Code Coverage Infrastructure: `docs/superpowers/specs/2026-04-04-firmware-code-coverage-design.md`
- PR #39 — QEMU Blackbox Test Framework (pending)
- PR #40 — Coverage discoverability (Makefile help + README)
- `docs/coverage.md` — coverage user guide
- `docs/QEMU_BLACKBOX_TESTING.md` — blackbox user guide (ships with PR #39)
