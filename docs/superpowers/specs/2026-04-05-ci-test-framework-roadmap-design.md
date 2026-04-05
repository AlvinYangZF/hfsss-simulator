# CI Test Framework Roadmap — Design Spec

**Date:** 2026-04-05
**Status:** Draft — awaiting review
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

## 2. Current State Assessment (as of 2026-04-05)

### 2.1 CI workflows

| Workflow | Triggers | What it runs |
|----------|----------|-------------|
| `.github/workflows/build.yml` | push, PR | ubuntu-latest build, macos-latest build, Linux kernel module build |
| `.github/workflows/coverage.yml` (PR #38) | push, PR | `make coverage-build` + `make coverage-ut` + ratchet check + PR comment |

### 2.2 Available Make targets

| Target | Runs | In CI? | Duration |
|--------|------|--------|----------|
| `make test` | 37 UT binaries | ❌ build-only | ~30 s |
| `make systest` | 3 systest binaries (~616 assertions) | ❌ | ~2 min |
| `make stress-long` | stress_stability | ❌ | 10–60 min |
| `make coverage-ut` | UT suite instrumented | ✅ | 2–3 min |
| `make coverage-e2e` | QEMU + fio instrumented | ❌ local only | 10–15 min |
| `make coverage-merge` | Union UT+E2E | ❌ | 5 s |
| `make qemu-blackbox-ci` (PR #39 pending) | 8 blackbox cases | ❌ no workflow | 8–15 min |

### 2.3 Gaps

- **No CI execution of `make test`** — builds produce binaries that are never run on PRs.
- **No `make systest` in CI** — system-level assertions unvalidated.
- **No blackbox E2E in CI** — PR #39 adds the framework but no workflow wires it.
- **No performance regression tracking** — `scripts/fio_verify_suite.sh` exists, blackbox fio cases produce JSON, but numbers are never compared across commits.
- **No sanitizer builds** — ASan / UBSan / TSan are widely available but unused.
- **No static analysis** — `cppcheck`, `clang-tidy`, `scan-build` not integrated.
- **No code-format enforcement** — contributor style drift.
- **Duplicate E2E harnesses** — PR #38's `run_e2e_coverage.sh` and PR #39's `env.sh` both boot QEMU + NBD + fio independently.

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

**Goal:** Make every already-written test actually run on every PR.

| PR | Deliverable | Prerequisite |
|----|-------------|--------------|
| S1-P1 | Add `make test` step to `.github/workflows/build.yml` (UT execution, not just build) | none |
| S1-P2 | Add `.github/workflows/systest.yml` running `make systest` on ubuntu-latest | S1-P1 |
| S1-P3 | Complete PR #39 + add `.github/workflows/blackbox.yml` (smoke subset only, per PR) | PR #39 merged |
| S1-P4 | Per-case timeout + namespace reset in blackbox `run.sh` | PR #39 merged |
| S1-P5 | Guest image publishing pipeline (GitHub Release + download step) | S1-P3 |
| S1-P6 | QEMU architecture abstraction (`HFSSS_QEMU_ARCH` / `HFSSS_QEMU_ACCEL`) for Linux CI runners | S1-P3 |

**Sprint 1 success criteria:**
- Every PR to master triggers `make test`, `make systest`, and blackbox smoke in CI.
- All three must pass to merge.
- Total added CI time: ≤ 15 min per PR.

### Sprint 2 — Performance regression

**Goal:** Prevent silent IOPS / latency regressions.

| PR | Deliverable |
|----|-------------|
| S2-P1 | `scripts/perf/parse_fio_json.sh` — extract iops / lat_mean / p99 from blackbox fio output |
| S2-P2 | `scripts/perf/ratchet_check.sh` — compare parsed metrics against `.perf-baseline.json`, reuse PR #38 ratchet pattern |
| S2-P3 | `.perf-baseline.json` seeded from master (4 fio workloads × 3 modes = 12 metrics) |
| S2-P4 | `post_perf_summary.sh` — PR comment with perf delta (reuses PR #38 marker idempotency) |
| S2-P5 | Extend `.github/workflows/blackbox.yml` with perf-ratchet step |
| S2-P6 | Nightly soak workflow (`.github/workflows/nightly.yml`) running full blackbox matrix |

**Sprint 2 success criteria:**
- PR IOPS regression > 20 % blocks merge.
- Regression 10–20 % warns in PR comment.
- Nightly soak runs 40 rounds × full matrix and emits trend report.

### Sprint 3 — Couple coverage and blackbox

**Goal:** Eliminate duplicate E2E harnesses and automate E2E coverage.

| PR | Deliverable |
|----|-------------|
| S3-P1 | Add `--coverage` flag to blackbox `run.sh` — switches to `build-cov/bin/hfsss-nbd-server`, runs lcov after suite |
| S3-P2 | Retire or wrap `scripts/coverage/run_e2e_coverage.sh` on top of blackbox framework |
| S3-P3 | New-code coverage ratchet — parse `git diff` + `diff_cover` against `ut.info` |
| S3-P4 | Merge coverage + blackbox PR comments into one unified quality comment |
| S3-P5 | Nightly merged coverage report (UT + E2E) uploaded as CI artifact |

**Sprint 3 success criteria:**
- `make coverage-e2e` and blackbox framework share the same QEMU lifecycle code.
- PR new code must be ≥ 80 % line-covered (in addition to the absolute ratchet).
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
