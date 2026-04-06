# HFSSS Code Coverage

## Quick Start

```bash
# Install lcov (one-time)
brew install lcov         # macOS
sudo apt install lcov     # Linux

# UT coverage only (fast — 1-3 min)
make coverage-ut
open build-cov/coverage/ut/index.html

# Full flow: UT + E2E + merged report (slow — 10-20 min)
# Requires QEMU image set up per scripts/start_nvme_test.sh prerequisites.
make coverage

# Run the coverage infra self-tests
make coverage-selftest
```

## Scope

**Included** (measured):
- `src/{common,media,hal,ftl,controller,pcie,perf}/*.c` + `src/sssim.c`
- inline functions in `include/`

**Excluded** (filtered via `lcov --remove`):
- `src/vhost/*` — NBD / vhost-user transport layer
- `src/kernel/*` — Linux kernel module (host driver)
- `src/tools/*` — CLI utilities
- `tests/*` — the tests themselves
- system headers

## Reports

After `make coverage`, three HTML reports are produced:

| Report | Path | What it shows |
|--------|------|--------------|
| UT | `build-cov/coverage/ut/index.html` | Coverage from `make test` + systests + short stress |
| E2E | `build-cov/coverage/e2e/index.html` | Coverage from fio running in QEMU guest through NBD |
| Merged | `build-cov/coverage/merged/index.html` | Union of UT + E2E |

**Use the merged report** to see overall coverage; use **UT vs E2E comparison** to find test blind spots (code only covered by one path).

## CI Ratchet

A `.coverage-baseline.json` tracks the reference coverage.
Every PR runs `make coverage-ut` in CI and **fails** if any metric
(line / function / branch) drops more than 2 percentage points below
the baseline.

### Raising the baseline (after improving coverage)

1. Merge your PR to master.
2. Pull latest master locally.
3. Run:
   ```bash
   make coverage-ut
   bash scripts/coverage/ratchet_check.sh --update-baseline
   git add .coverage-baseline.json
   git commit -m "chore: update coverage baseline"
   git push
   ```

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `lcov: ERROR: stamp mismatch` | Source changed but old `.gcno` remains | `make coverage-clean && make coverage-build` |
| Coverage shows 0% after run | Test binary crashed before flush, OR `.gcda` not written | Check stderr of failing binary; check disk space; rerun |
| `ut/index.html` shows `src/vhost/` | Exclusion filter missed | Check lcov version ≥ 1.14; path separator `/src/vhost/` |
| E2E hangs at "Waiting for SSH" | QEMU guest not booting | Check `/tmp/cov_qemu.log` and `scripts/start_nvme_test.sh` prerequisites |
| Ratchet fails on first PR | No baseline yet | First run bootstraps; commit `.coverage-baseline.json` |
| Branch coverage is noisy | `-O0` exposes compiler-generated branches | Accept for now; documented in spec Open Questions |

## Design

See `docs/superpowers/specs/2026-04-04-firmware-code-coverage-design.md`

## Focused Follow-Up

For the current front-end and control-plane coverage gap analysis, plus the
recommended CI landing order for closing those gaps, see:

- `docs/COVERAGE_FRONTEND_IMPROVEMENT_PLAN.md`
