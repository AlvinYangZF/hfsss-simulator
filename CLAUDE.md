# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview
HFSSS (High Fidelity Full-Stack SSD Simulator) is a complete SSD simulation project that includes full-stack simulation from PCIe/NVMe interface to NAND Flash media.

## Working Method
- Explain the approach before implementing.
- If requirements are ambiguous, high-risk, or high-impact, clarify and wait for approval before writing any code.
- In Plan mode, write proposals only — no code.
- Stick to Spec Coding; no Vibe Coding.
- Prioritize iteration using /loop.
- Run /simplify when done.

## Coding Style
- C style with Google Coding Style:https://google.github.io/styleguide/cppguide.html

## Coding Rules

- Only English is allowed in code.
- Specs must not rely on line numbers to locate code.
- Do not write process-oriented explanations in comments.
- Prefer conceptual descriptions to locate code; avoid "file path + line number."

## Decomposition & Scope Control

- Break tasks into loosely coupled, independently verifiable subtasks; use /batch when necessary.
- Any process that repeats 3 times should be formalized as a Skill.

## Quality Requirements

- In the early stages of a project, keep only the minimum necessary quality standards: runnable, verifiable, and rollback-able.
- Prioritize making critical paths and high-risk changes verifiable.
- When handling bugs: reproduce first, then fix and verify.

## Error Correction & Collaboration

- When corrected, identify the root cause and improve your approach; for recurring issues, formalize them into explicit rules.
- Separate implementation from review: complete the proposal or code first, then review it independently.

## Prohibited

- Never use /init.
- CLAUDE.md should be written based on the project's actual needs — do not copy generic templates.
- Avoid terms that describe development progress (FIXED, Step, Week, Section, Phase, AC-x, etc.) in code comments, commit messages, or PR bodies.
- Avoid AI tool names (e.g. Codex, Claude, Grok, Gemini, …) in code comments, git commit messages (including authorship), or PR bodies.

## Build Commands
- `make all` - Build all libraries and test programs
- `make test` - Build and run all tests
- `make clean` - Clean build directory

## CI/CD
GitHub Actions is set up to automatically build and test on every push and pull request.

## Project Structure
```
.
├── include/           # Header files
│   ├── common/       # Common service headers
│   ├── media/        # Media simulation headers
│   ├── hal/          # HAL headers
│   ├── ftl/          # FTL headers
│   ├── controller/   # Controller headers
│   ├── pcie/         # PCIe/NVMe headers
│   └── sssim.h       # Top-level interface
├── src/              # Source code
├── tests/            # Test files
├── docs/             # Design documents
└── Makefile          # Build system
```

## Test Status
All 992+ tests pass as of the latest build.
