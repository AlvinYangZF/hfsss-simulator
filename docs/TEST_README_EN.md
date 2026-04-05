# High-Fidelity Full-Stack SSD Simulator (HFSSS) Test Design Document Overview

**Document Version**: V1.0
**Date**: 2026-03-08

---

## Table of Contents

1. [Test Design Document Overview](#1-test-design-document-overview)
2. [Test Document List](#2-test-document-list)
3. [Test Strategy Overview](#3-test-strategy-overview)
4. [Test Execution Flow](#4-test-execution-flow)
5. [Test Toolchain](#5-test-toolchain)
6. [Quality Standards](#6-quality-standards)
7. [Test Result Report Template](#7-test-result-report-template)

---

## 1. Test Design Document Overview

This series of test design documents provides comprehensive test plans for each LLD detailed design document, including:
- Unit Test Design (function-level white-box testing)
- Functional Test Case Design (module-level black-box testing)
- Integration Test Design (inter-module interaction testing)
- Performance Test Design (performance benchmarking)
- Boundary Condition Test Design
- Exception Test Design
- Regression Test Design (quick/full/nightly)
- Before Check In Test Design (pre-submission checks)
- Code Coverage Statistics (using tools such as gcov/lcov)
- Test Tools and Environment Configuration
- Test Execution Plan

All test documents provide quantifiable test result metrics and support quick-verification regression test plans.

---

## 2. Test Document List

| Test Document | Corresponding LLD | Word Count | Unit Test Cases |
|---------------|-------------------|------------|-----------------|
| TEST_LLD_01_PCIE_NVMe_EMULATION.md | LLD_01 | ~35,000 | 200+ |
| TEST_LLD_02_CONTROLLER_THREAD.md | LLD_02 | ~32,000 | 180+ |
| TEST_LLD_03_MEDIA_THREADS.md | LLD_03 | ~31,000 | 150+ |
| TEST_LLD_04_HAL.md | LLD_04 | ~30,000 | 100+ |
| TEST_LLD_05_COMMON_SERVICE.md | LLD_05 | ~33,000 | 200+ |
| TEST_LLD_06_APPLICATION.md | LLD_06 | ~35,000 | 220+ |
| **Total** | - | **~196,000** | **1,050+** |

---

## 3. Test Strategy Overview

### 3.1 Test Pyramid

```
                    /\
                   /  \     <-- End-to-End Tests (E2E)
                  /    \
                 /      \   <-- Integration Tests
                /        \
               /          \ <-- Functional Tests
              /            \
             /              \
            /________________\ <-- Unit Tests (Foundation)
```

### 3.2 Test Types

| Test Type | Execution Timing | Owner | Execution Time |
|-----------|-----------------|-------|----------------|
| Unit Tests | Every coding session | Development Engineer | < 5 minutes |
| Functional Tests | Every submission | Test Engineer | < 30 minutes |
| Integration Tests | Daily build | Test Engineer | < 2 hours |
| Performance Tests | Weekly build | Performance Engineer | < 24 hours |
| Stress Tests | Milestones | Test Engineer | 7x24 hours |

### 3.3 Code Coverage Targets

| Coverage Type | Target | Measurement Tool |
|---------------|--------|------------------|
| Line Coverage | >= 90% | gcov/lcov |
| Function Coverage | >= 95% | gcov/lcov |
| Branch Coverage | >= 85% | gcov/lcov |

---

## 4. Test Execution Flow

### 4.1 Implemented Build & Test Flow

The current test execution is orchestrated by the root `Makefile`.

```
Developer runs "make test"
    |
+---1. Build Static Libraries: All source files in `src/*` are compiled
|      into static libraries (e.g., `libhfsss-ftl.a`) in `build/lib/`.
|
+---2. Build Test Executables: Each `tests/test_*.c` and `tests/systest_*.c`
|      file is compiled into a separate executable in `build/bin/`,
|      linking against the required static libraries.
|
+---3. Run Test Executables: The `test` target in the Makefile executes
       each test program sequentially (e.g., `build/bin/test_common`,
       `build/bin/test_ftl`, etc.).

Each test executable prints its results to standard output and exits with a
non-zero status code upon failure, which halts the `make` process.
```

---

## 5. Test Toolchain

### 5.1 Implemented Toolchain

The core test system relies on a minimal set of standard build tools.

| Tool | Purpose |
|------|---------|
| `make` | The primary test runner and build orchestrator. |
| `gcc` | Compiles the source code and test executables. |
| `ar` | Creates the static libraries for each module. |
| Internal C Macros | A simple, custom `TEST_ASSERT` macro within each test file is used for assertions. |

### 5.2 Aspirational Toolchain

_The following tools are listed in project planning documents but are **not** currently integrated into the automated `make test` flow. They represent future goals for improving the test infrastructure._

| Tool | Purpose |
|------|---------|
| CUnit/GTest | Standardized unit test frameworks. |
| gcov/lcov | Code coverage reporting. |
| cppcheck/clang-analyzer | Static analysis tools. |
| fio/perf | Performance and I/O benchmarking tools. |
| Jenkins/GitHub Actions | Continuous Integration (CI/CD) pipelines. |

---

## 6. Quality Standards

### 6.1 Test Pass Criteria

| Metric | Requirement |
|--------|-------------|
| Unit Test Pass Rate | 100% |
| Functional Test Pass Rate | 100% |
| Integration Test Pass Rate | 100% |
| Code Line Coverage | >= 90% |
| Code Function Coverage | >= 95% |
| Compiler Warnings | 0 |
| Static Analysis Critical Errors | 0 |

### 6.2 Bug Severity Levels

| Level | Definition | Fix SLA |
|-------|------------|---------|
| Critical | System crash, data loss | 24 hours |
| Major | Major functionality unavailable | 3 days |
| Minor | Minor functionality issue | 1 week |
| Enhancement | Feature improvement suggestion | Next release |

---

## 7. Test Result Report Template

### 7.1 Daily Test Report

```
=============================================
HFSSS Daily Test Report
Date: YYYY-MM-DD
Build ID: BUILD-XXX
=============================================

1. Overall Status: [PASS/FAIL]

2. Unit Tests:
   Total Cases: XXX
   Passed: XXX
   Failed: XXX
   Pass Rate: XX.X%

3. Functional Tests:
   Total Cases: XXX
   Passed: XXX
   Failed: XXX
   Pass Rate: XX.X%

4. Integration Tests:
   Total Cases: XXX
   Passed: XXX
   Failed: XXX
   Pass Rate: XX.X%

5. Code Coverage:
   Line Coverage: XX.X%
   Function Coverage: XX.X%
   Branch Coverage: XX.X%

6. Failed Test Cases:
   - [TEST-ID] Description

7. Issues and Risks:
   - Issue 1: Description
   - Risk 1: Description

=============================================
Report Generated: HH:MM:SS
=============================================
```

---

## Appendix

### A. Quick Reference Commands

```bash
# Run Before Check In checks
./before_checkin.sh

# Run unit tests
./run_unit_tests.sh

# Run functional tests
./run_functional_tests.sh

# Run integration tests
./run_integration_tests.sh

# Run performance tests
./run_perf_tests.sh

# Generate code coverage report
./run_coverage.sh

# Run full regression tests
./run_regression.sh full
```

---

**Document Statistics**:
- Total test design documents: 6 + 1 overview
- Total word count: approximately 196,000 words
- Total unit test cases: 1,050+
