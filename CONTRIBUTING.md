# Contributing to HFSSS

## Pre-checkin gate (mandatory)

**Every commit, every PR, every agent — no exceptions.**

Before `git commit`, before opening a PR, before handing a branch to
another agent: run the pre-checkin gate and confirm it passes.

```
make pre-checkin
```

This runs the full QEMU blackbox bundle through a real Linux guest
against the instrumented HFSSS NBD server, including a calibrated 8 GiB
mixed read/write stress case with CRC32C block-level verification.
Wall-clock: ~8-12 minutes on Mac Studio.

**Pass criteria:** exit code 0 and the trailer `pre-checkin PASS —
ready to commit`. Anything else is a FAIL — do not commit.

**No skip path.** "I don't have QEMU" is not an acceptable excuse.
The only legitimate bypass is a genuine infrastructure outage
plus explicit written owner sign-off in the PR thread.

**Paste the PASS banner + timing into your PR description.** PRs
without that evidence are not eligible for review.

Full policy, rationale, and setup instructions:
[`docs/PRE_CHECKIN_STANDARD.md`](docs/PRE_CHECKIN_STANDARD.md).

## Other references

- [`docs/CI_RUN_ISOLATION.md`](docs/CI_RUN_ISOLATION.md) — how to run
  CI safely alongside other agents on the same host.
- [`docs/superpowers/specs/2026-04-05-ci-test-framework-roadmap-design.md`](docs/superpowers/specs/2026-04-05-ci-test-framework-roadmap-design.md)
  — the 5-pillar CI quality framework this gate sits inside.
- [`docs/VHOST_USER_GUIDE.md`](docs/VHOST_USER_GUIDE.md) — the
  vhost-user-blk integration path.
