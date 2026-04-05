# HFSSS Firmware Code Coverage Design

**Date:** 2026-04-04
**Status:** Design Approved — Awaiting Implementation Plan
**Author:** Claude Code (brainstormed with AlvinYangZF)

---

## 1. Overview

Add code coverage measurement infrastructure to HFSSS, producing three visual HTML reports (UT-only, E2E-only, merged) that quantify how thoroughly the firmware is exercised by the existing test suites. A ratchet-based coverage floor is enforced in CI (UT path only) to prevent regressions.

**Goals:**
1. Measure coverage of HFSSS **firmware** code only (exclude transport-layer glue, kernel modules, CLI tools, and the tests themselves).
2. Use two existing test channels as coverage sources:
   - **UT**: the ~45 `test_*` / `stress_*` / `systest_*` binaries driven by `make test` and `make systest`.
   - **E2E**: the QEMU + NBD + fio verification suite (`scripts/start_nvme_test.sh` + `scripts/fio_verify_suite.sh`).
3. Produce three separate HTML reports so gaps attributable to UT vs. E2E can be identified independently.
4. Enforce a ratchet in CI on UT coverage only (first run sets baseline, floor = baseline − 2% for line / function / branch).
5. Zero impact on the existing `-O2` production build — coverage is a parallel build variant.

**Non-goals:**
- E2E coverage is **not** gated in CI (too slow, env-heavy). It runs on-demand locally or as a nightly job.
- No absolute floor is picked without empirical baseline data.
- No new-code-per-patch threshold (scope kept minimal; can be added later).

---

## 2. Scope — What Counts as "Firmware"

### 2.1 Included (measured)

| Path | Role |
|------|------|
| `src/common/*.c` | Infrastructure: mempool, mutex, log, watchdog, UPLP, telemetry |
| `src/media/*.c` | NAND / media simulation |
| `src/hal/*.c` | Hardware abstraction layer, channel scheduler, EAT model |
| `src/ftl/*.c` | FTL core: block manager, mapping (TAA), GC, WL, superblock, T10-PI |
| `src/controller/*.c` | NVMe controller, SQ/CQ, admin/IO command parsing |
| `src/pcie/*.c` | PCIe config space, BAR, doorbell, PRP/SGL |
| `src/perf/*.c` | Performance model / QoS |
| `src/sssim.c` | Top-level simulator entry point |
| `include/**/*.h` inline functions | Inline FTL/HAL helpers still count as firmware logic |

### 2.2 Excluded (removed via `lcov --remove` after capture)

| Path | Rationale |
|------|-----------|
| `src/vhost/*.c` | NBD server + vhost-user-blk glue — user-explicit exclusion (transport layer, not firmware) |
| `src/kernel/*.c` | Linux kernel module — runs in host kernel space, not firmware runtime |
| `src/tools/*.c` | CLI tools (hfsss-ctrl) — utilities, not firmware runtime |
| `tests/*.c` | The tests themselves |
| System headers (`/usr/include/**`) | Standard library / libc |

**Note:** `src/kernel` and `src/tools` exclusions are my added interpretation of "firmware only". If the user wants them included, adjust Section 9 of the implementation plan.

---

## 3. Architecture

### 3.1 Parallel Build Variant

Coverage builds live in `build-cov/`, fully isolated from the existing `-O2` `build/` tree. The same Makefile drives both variants via a `COVERAGE=1` switch.

```
hfsss-simulator/
├── build/                       # Existing: -O2 optimized (perf testing)
│   ├── lib/ bin/
├── build-cov/                   # NEW: -O0 --coverage instrumented
│   ├── lib/   bin/              # each .o sits next to its .gcno
│   └── coverage/
│       ├── ut/index.html        # UT-only report
│       ├── e2e/index.html       # E2E-only report
│       ├── merged/index.html    # Merged report
│       ├── ut.info              # lcov tracefile
│       ├── e2e.info
│       └── merged.info
├── .coverage-baseline.json      # NEW: persisted ratchet baseline
└── scripts/
    └── coverage/                # NEW
        ├── run_ut_coverage.sh
        ├── run_e2e_coverage.sh
        ├── merge_reports.sh
        ├── ratchet_check.sh
        ├── post_summary.sh
        └── reset_counters.sh
```

### 3.2 Makefile Integration

New targets and variable:

```make
# Coverage build variant switch
ifeq ($(COVERAGE),1)
    CFLAGS  += --coverage -O0 -g
    LDFLAGS += --coverage
    BUILD_DIR := build-cov
endif

.PHONY: coverage-build coverage-ut coverage-e2e coverage-merge coverage coverage-clean

coverage-build:
	$(MAKE) COVERAGE=1 all

coverage-ut: coverage-build
	bash scripts/coverage/reset_counters.sh
	bash scripts/coverage/run_ut_coverage.sh

coverage-e2e: coverage-build
	bash scripts/coverage/reset_counters.sh
	bash scripts/coverage/run_e2e_coverage.sh

coverage-merge:
	bash scripts/coverage/merge_reports.sh

# Full flow: build → UT → E2E → merge (local only)
coverage: coverage-ut coverage-e2e coverage-merge

coverage-clean:
	rm -rf build-cov
	find . -name '*.gcda' -delete
```

- `make test` and `make all` continue to work unchanged (no `COVERAGE=1`).
- `make coverage` is a **local-only** full flow (requires QEMU).
- CI calls `make coverage-ut` only.

### 3.3 Data Flow

```
gcc --coverage on src/ → .o + .gcno  (at build time, one per compilation unit)
                             │
                             ▼
                    test binary runs
                             │
                             ▼
                          .gcda      (at exit, flushed next to .gcno)
                             │
                             ▼
          lcov -c -d build-cov -o raw.info
                             │
                             ▼
     lcov --remove raw.info  '*/src/vhost/*' '*/src/kernel/*'
                             '*/src/tools/*' '*/tests/*'
                             '/usr/include/*'  -o clean.info
                             │
                             ▼
                    genhtml clean.info → HTML
```

---

## 4. Components

### 4.1 `scripts/coverage/reset_counters.sh`

Before each test run, zero out any stale `.gcda` files so runs don't accumulate across UT and E2E phases when we want them separated.

```bash
#!/bin/bash
# Delete all .gcda files but keep .gcno (structure) intact
find build-cov -name '*.gcda' -delete
```

### 4.2 `scripts/coverage/run_ut_coverage.sh`

1. Reset `.gcda` counters.
2. Run every test binary in `build-cov/bin/test_*` and `build-cov/bin/systest_*` and a short-duration `stress_*` pass (STRESS_DURATION=5s).
3. Capture `.gcda` → `build-cov/coverage/ut.info` via `lcov -c -d build-cov`.
4. Apply `--remove` filters.
5. Generate HTML via `genhtml build-cov/coverage/ut.info -o build-cov/coverage/ut/`.
6. Print line/function/branch summary to stdout.

**Included binaries:** all `test_*`, `systest_*`, and `stress_*` binaries listed in Makefile targets under the `test:` and `systest:` recipes.

**Explicitly stress tests:** run with `STRESS_DURATION=5` to keep CI fast (they otherwise default to 60s).

### 4.3 `scripts/coverage/run_e2e_coverage.sh`

1. Reset `.gcda` counters.
2. Launch `build-cov/bin/hfsss-nbd-server -a &` (async mode) in background.
3. Start QEMU via `scripts/start_nvme_test.sh` (talks to NBD server).
4. SSH into guest, run `scripts/fio_verify_suite.sh` which executes the fio test suite.
5. SIGTERM the NBD server gracefully so it flushes `.gcda` at exit.
6. Capture `.gcda` → `build-cov/coverage/e2e.info` via lcov.
7. Apply same `--remove` filters.
8. Generate HTML via `genhtml build-cov/coverage/e2e.info -o build-cov/coverage/e2e/`.

**Graceful shutdown matters:** gcov writes `.gcda` only at normal process exit or `__gcov_dump()` call. `SIGKILL` loses data. The NBD server already has a clean SIGTERM path via its accept loop; use `kill -TERM <pid> && wait`.

### 4.4 `scripts/coverage/merge_reports.sh`

```bash
lcov -a build-cov/coverage/ut.info \
     -a build-cov/coverage/e2e.info \
     -o build-cov/coverage/merged.info
genhtml build-cov/coverage/merged.info -o build-cov/coverage/merged/
```

Runs after both `ut.info` and `e2e.info` exist. `lcov -a` (add-tracefile) performs a union merge: a line covered in either source is marked covered in output.

### 4.5 `scripts/coverage/ratchet_check.sh`

```bash
# Parse line/func/branch % from lcov --summary output
# Compare against .coverage-baseline.json (tracked in git)
# If any metric drops > 2%, exit 1 (CI fails)
# If no baseline exists, write current values as baseline and exit 0
```

**Baseline file format** (`.coverage-baseline.json`):
```json
{
  "created_at": "2026-04-04T12:00:00Z",
  "commit": "bfa6157",
  "metrics": {
    "line":     { "baseline": 62.5, "floor": 60.5 },
    "function": { "baseline": 78.1, "floor": 76.1 },
    "branch":   { "baseline": 48.3, "floor": 46.3 }
  }
}
```

**Ratchet update policy:** The baseline is **not** auto-updated on every CI run. It is updated only when the user explicitly runs `scripts/coverage/ratchet_check.sh --update-baseline` after a PR that intentionally raises coverage, then commits the modified JSON. This prevents silent drift.

**Floor = baseline − 2%** for each metric.

### 4.6 `scripts/coverage/post_summary.sh`

Posts or updates a PR comment summarizing coverage (line / function / branch %, delta vs. baseline, artifact link). Uses `gh pr comment` with a marker string (`<!-- coverage-bot -->`) so repeated runs update the same comment instead of spamming new ones. Only invoked by CI when `GITHUB_EVENT_NAME=pull_request`.

### 4.7 New Makefile Targets (summary)

| Target | What it does | Runs in CI |
|--------|-------------|-----------|
| `make coverage-build` | Build firmware with `--coverage -O0` | ✅ (via coverage-ut) |
| `make coverage-ut` | Build + run all UT + produce `ut/` HTML | ✅ |
| `make coverage-e2e` | Build + QEMU fio + produce `e2e/` HTML | ❌ (local only) |
| `make coverage-merge` | Merge ut + e2e → `merged/` HTML | ❌ (local only) |
| `make coverage` | Full flow (ut → e2e → merge) | ❌ (local only) |
| `make coverage-clean` | Delete build-cov + all .gcda | Manual |

---

## 5. CI Integration

### 5.1 GitHub Actions Workflow

New file: `.github/workflows/coverage.yml`

```yaml
name: Coverage
on:
  pull_request:
    branches: [master]
  push:
    branches: [master]

jobs:
  ut-coverage:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install deps
        run: sudo apt-get update && sudo apt-get install -y lcov
      - name: Build instrumented
        run: make coverage-build
      - name: Run UT coverage
        run: make coverage-ut
      - name: Ratchet check
        run: bash scripts/coverage/ratchet_check.sh
      - name: Upload HTML report
        uses: actions/upload-artifact@v4
        with:
          name: coverage-ut-html
          path: build-cov/coverage/ut/
      - name: Post summary to PR
        if: github.event_name == 'pull_request'
        run: bash scripts/coverage/post_summary.sh
```

### 5.2 PR Summary Comment

`scripts/coverage/post_summary.sh` posts a comment on the PR showing:
- Current line/func/branch % vs. baseline and floor.
- Delta from base branch (if computable).
- Link to downloadable HTML artifact.

**Idempotency:** the script updates an existing bot comment (identified by a marker string) instead of creating a new one each push.

### 5.3 First-Run Bootstrap

On the first CI run (when `.coverage-baseline.json` does not exist), `ratchet_check.sh`:
1. Computes current coverage.
2. **Writes `.coverage-baseline.json` locally but does NOT commit it** (CI runner has no push rights).
3. Prints a message telling the user to run `make coverage-ut && bash scripts/coverage/ratchet_check.sh --update-baseline` locally, then commit the JSON.
4. Exits 0 (doesn't block first PR).

---

## 6. Error Handling & Edge Cases

| Case | Handling |
|------|---------|
| `.gcda` write fails (e.g., read-only filesystem) | Instrumented binary prints warning to stderr; `.info` will show 0% for that file; `lcov -c` continues with warning |
| Test binary crashes (SIGSEGV) | `.gcda` not written for that process. Run continues; coverage for crashed binary's unique paths is lost for that run. UT suite still proceeds because each binary is invoked independently via `||true` wrapper. |
| `.gcda` / `.gcno` version mismatch (source changed but `.gcno` stale) | `lcov -c` aborts with "stamp mismatch" — fix: `make coverage-clean && make coverage-build`. Documented in troubleshooting section of README. |
| QEMU fails to start in E2E | `run_e2e_coverage.sh` catches non-zero exit from `start_nvme_test.sh`, kills any leftover QEMU/nbd-server processes, exits with error. No partial `.gcda` merged. |
| NBD server killed with SIGKILL | `.gcda` lost. Scripts explicitly use SIGTERM and `wait $pid` (with 10s timeout before escalating to SIGKILL). |
| lcov `--remove` pattern doesn't match (path format variation) | `scripts/coverage/run_ut_coverage.sh` ends with a grep assertion: `grep -q 'SF:.*src/vhost' clean.info && echo "ERROR: vhost not excluded" && exit 1` |
| Parallel `make -jN` coverage build | gcov handles this: `.gcno` is written at compile time and is unique per source. Safe. |
| Multiple test processes writing `.gcda` simultaneously | gcov runtime library handles concurrent append via file locking. Safe on Linux. |

---

## 7. Testing the Coverage Infrastructure Itself

The coverage feature needs its own tests (meta-testing):

1. **Smoke test:** `make coverage-build` succeeds; at least one `.gcno` file exists under `build-cov/`.
2. **UT coverage test:** `make coverage-ut` produces `build-cov/coverage/ut/index.html` and a non-empty `ut.info`.
3. **Exclusion test:** assert that `build-cov/coverage/ut.info` contains zero lines matching `SF:.*src/vhost/` (grep-based assertion baked into `run_ut_coverage.sh`).
4. **Inclusion test:** assert `ut.info` DOES contain `src/ftl/` and `src/sssim.c` entries.
5. **Merge test:** create synthetic `a.info` and `b.info`, merge, verify union semantics.
6. **Ratchet test:** unit test `ratchet_check.sh` with a crafted baseline JSON and crafted lcov summary — verify pass/fail under threshold.
7. **First-run bootstrap test:** delete `.coverage-baseline.json`, run `ratchet_check.sh`, verify it creates the file and exits 0.

These tests live under `scripts/coverage/tests/` and are invoked via a new `make coverage-selftest` target (optional, can be run locally).

---

## 8. Expected Outcomes

After implementation:

- Developer runs `make coverage` locally → gets three HTML reports in ~5-15 min.
- Developer runs `make coverage-ut` locally → gets UT report in ~1-2 min.
- Every PR runs `make coverage-ut` in CI (~2-4 min) and posts a comment with metrics.
- Coverage cannot silently regress by more than 2% on any metric.
- Gaps visible: the merged report shows un-covered firmware lines, and per-report comparison shows which lines are UT-only or E2E-only.

---

## 9. Open Questions / Risks

| # | Item | Owner | Resolution Plan |
|---|------|-------|----------------|
| 1 | Confirm `src/kernel` and `src/tools` exclusion (added by Claude, not user-explicit) | User | User confirms in spec review |
| 2 | Confirm ratchet floor delta is 2% (not 1% / 3%) | User | User confirms in spec review |
| 3 | CI runner cost: `make coverage-ut` adds ~2-4 min per PR | Acceptable | Low cost, documented |
| 4 | `.coverage-baseline.json` update workflow relies on human discipline | Acceptable | Documented; later could add bot auto-update |
| 5 | Branch coverage with `-O0 --coverage` can be noisy (short-circuit eval, compiler-generated branches) | Monitor | Accept for baseline, track false positives in first month |
| 6 | Stress tests with `STRESS_DURATION=5` may under-cover — some bugs need longer runs | Acceptable | Nightly job can run full-duration stress with coverage merged into E2E report |

---

## 10. Success Criteria

1. `make coverage-build` compiles all firmware sources with `--coverage -O0` into `build-cov/`.
2. `make coverage-ut` produces a working HTML report at `build-cov/coverage/ut/index.html`.
3. `make coverage-e2e` produces a working HTML report at `build-cov/coverage/e2e/index.html`.
4. `make coverage-merge` produces a merged HTML report.
5. All four report trees exclude `src/vhost/`, `src/kernel/`, `src/tools/`, `tests/`, and system headers — verified by grep assertion.
6. CI workflow runs `make coverage-ut` + ratchet check on every PR.
7. First PR writes `.coverage-baseline.json` and passes; subsequent PRs fail if any metric drops > 2% below baseline.
8. PR comment contains line/func/branch % and delta vs. baseline.
9. `make all` and `make test` (existing flows) continue to work unchanged.
