# AGENT.md

This file is the authoritative instruction set for agents working in the
`hfsss-simulator` repository. If another agent guidance file exists, follow this
file unless the user gives a more specific instruction in the current session.

## Repository Scope

HFSSS is a full-stack SSD simulator written in C. The codebase spans:

- NVMe and PCIe user-space interfaces
- SSD controller logic
- FTL, mapping, GC, wear leveling, ECC, and recovery
- HAL and NAND media simulation
- Stress, system, and unit tests
- Investigation tooling under `scripts/`, `tools/`, and `docs/superpowers/`

The layering matters. Keep changes localized and preserve interface boundaries
between `include/`, `src/`, `tests/`, and `docs/`.

## Operating Principles

- Explain the intended approach before implementing meaningful changes.
- If a request is ambiguous, high-risk, or high-impact, clarify the constraint
  or approval boundary before writing code.
- In plan-only workflows, write proposals only and do not write code.
- Prefer spec-driven work for multi-step, high-risk, or architecture-affecting
  changes. For small, well-scoped fixes, proceed directly once the scope is
  clear.
- Keep changes independently verifiable. Break large work into loosely coupled
  slices when possible.
- If the same manual process repeats several times, turn it into a script,
  helper, or explicit repository rule.
- Separate implementation from review. Finish the change first, then review it
  with fresh eyes.
- When fixing bugs, reproduce first when practical, then fix, then verify.

## Language And Communication

- Use English only in agent outputs for this repository.
- Use English only in PR comments and review comments.
- Use English only in newly added code comments, documentation edits, and test
  descriptions unless the user explicitly requests otherwise.
- Do not use AI tool names in code comments, commit messages, or PR bodies.

## Coding Style

- Use C11 and follow Google-style C/C++ formatting conventions where they fit
  this codebase.
- Keep comments conceptual and technical. Do not write process-oriented comments
  such as "step 1", "fixed", "phase", or progress markers.
- Specs and design docs must not rely on line numbers to locate code. Use
  conceptual locations instead.
- Favor narrow, explicit interfaces over cross-layer shortcuts.

## Build And Test Workflow

Use the repository's Makefile first. Default entry points:

- `make all` builds libraries, binaries, and standard test targets
- `make test` builds and runs the unit and integration suite wired into the
  default test target
- `make clean` removes build output

When working on specific areas, prefer targeted verification in addition to the
broad suite:

- `build/bin/test_*` for focused module validation
- `build/bin/stress_*` for sustained-path validation when relevant
- `build/bin/hfsss-nbd-server` for NBD-path work

Trace-specific work must use the trace build variant:

- `TRACE=1 make all`
- `TRACE=1 make test`

Expect trace-enabled binaries in `build-trace/` when the Makefile routes them
there.

## High-Risk Areas

Treat these as high-risk even when the diff is small:

- `src/ftl/` multi-threaded write path, TAA interaction, GC, and recovery
- `src/vhost/hfsss_nbd_server.c` request lifecycle, cleanup ordering, and
  buffer sizing
- Any lifecycle that combines worker threads, HAL ownership, device cleanup,
  and trace teardown
- Any change that claims to be "tooling only" while also changing runtime
  behavior

For these areas, verify shutdown symmetry, ownership, and cleanup order
explicitly.

## Tooling And Diagnostic Changes

If you add or modify diagnostics, tracing, harnesses, or scripts:

- Ensure the runtime seam actually exists, not just the instrumentation points.
- Ensure the artifact consumer has a real producer path.
- Verify the error taxonomy is strict enough to avoid silent PASS conditions.
- Add or extend focused tests for the diagnostic behavior itself.
- Do not claim an end-to-end diagnostic pipeline works unless a real artifact
  was produced and consumed.

If you add runtime fixes alongside tooling, describe the PR as mixed-scope
instead of "tooling only".

## Review Expectations

When asked to review:

- Lead with findings, ordered by severity.
- Prioritize correctness, lifecycle safety, cleanup symmetry, data integrity,
  and testability over style-only comments.
- Cite files and functions precisely.
- Distinguish clearly between:
  - runtime behavior changes
  - test-only changes
  - tooling or docs changes

If no material findings remain, say so explicitly and note residual risk.

## Documentation Conventions

Use the existing documentation structure instead of inventing new top-level
patterns:

- `docs/superpowers/specs/` for design specs
- `docs/superpowers/plans/` for implementation plans and closeout notes
- `docs/superpowers/artifacts/` only for curated, review-relevant artifacts

Do not commit incidental runtime output unless the artifact is intentionally part
of the review record.

## Repository Guardrails

- Do not replace project-specific guidance with a generic template.
- Do not weaken repository instructions by splitting authority across multiple
  guidance files.
- Keep `AGENT.md` authoritative and keep compatibility files such as
  `CLAUDE.md` as pointers only.
