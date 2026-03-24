# HFSSS Documentation Standards

**Document Version**: V1.0
**Date**: 2026-03-23

---

## 1. Language

- All documentation must be written in English.
- Technical terms follow NVMe, JEDEC, OCP, and TCG standard terminology.
- Chinese-language source documents (without `_EN` suffix) are retained as reference but are not the authoritative version. The `_EN` variants are authoritative.
- No Chinese characters (Unicode range U+4E00--U+9FFF) may appear in any `_EN` file or in files intended for the English documentation set (e.g., `TRACEABILITY_MATRIX.md`, `REQUIREMENT_COVERAGE.md`).

---

## 2. Document Structure

All HLD, LLD, and TEST documents must include the following sections (in order):

### 2.1 HLD Document Template

1. **Title and metadata** (document name, module name)
2. **Revision History** table (Version, Date, Author, Description)
3. **Table of Contents** (numbered section links)
4. **Overview / Scope** (what the module does, boundaries)
5. **Requirements Traceability** table (REQ-ID, description, priority, status)
6. **Architecture / Design** sections (diagrams, data flow, interfaces)
7. **Architecture Decision Records (ADR)** (numbered ADR-001, ADR-002, etc.)
8. **References** (standards, related documents)

### 2.2 LLD Document Template

1. **Title and metadata**
2. **Revision History** table
3. **Table of Contents**
4. **Overview / Scope** (purpose, referenced HLD, covered requirements)
5. **Data Structures** (C struct definitions, constants, enums)
6. **API / Function Interface** specifications
7. **Internal Logic and Flow** (state machines, algorithms, sequence diagrams)
8. **Requirements Traceability** table
9. **Architecture Decision Records (ADR)**
10. **References**

### 2.3 TEST Document Template

1. **Title and metadata**
2. **Revision History** table
3. **Table of Contents**
4. **Overview / Scope** (test strategy, referenced LLD)
5. **Test Case Tables** (Test ID, Description, Steps, Expected Result, Priority)
6. **Requirements Coverage** mapping (Test ID to REQ-ID)
7. **References**

---

## 3. Naming Conventions

### 3.1 File Naming

| Document Type | Pattern | Example |
|---------------|---------|---------|
| High-Level Design | `HLD_NN_MODULE_NAME_EN.md` | `HLD_01_PCIE_NVMe_EMULATION_EN.md` |
| Low-Level Design | `LLD_NN_MODULE_NAME_EN.md` | `LLD_17_POWER_LOSS_PROTECTION_EN.md` |
| Test Specification | `TEST_LLD_NN_MODULE_NAME_EN.md` | `TEST_LLD_01_PCIE_NVMe_EMULATION_EN.md` |
| System Test Plan | `SYSTEM_TEST_PLAN_EN.md` | |
| Requirements Matrix | `REQUIREMENTS_MATRIX_EN.csv` | |
| PRD | `SSD_Simulator_PRD_EN.md` | |

- `NN` is a two-digit zero-padded sequence number (01--19).
- Module names use UPPER_SNAKE_CASE.
- English documents always end with `_EN` before the file extension.

### 3.2 REQ-ID Format

- Format: `REQ-NNN` (three digits, zero-padded). Examples: `REQ-001`, `REQ-042`, `REQ-178`.
- Enterprise requirement IDs continue the sequence from core requirements (REQ-139 onward).

### 3.3 Test Case ID Format

| Test Document | Prefix | Example |
|---------------|--------|---------|
| TEST_LLD_01 | `UT_PCIE_*`, `UT_NVME_*`, `UT_QUEUE_*` | `UT_PCIE_001` |
| TEST_LLD_02 | `UT_ARB_*`, `UT_SCHED_*`, `UT_WB_*` | `UT_ARB_010` |
| TEST_LLD_03 | `UT_NAND_*`, `UT_TIMING_*` | `UT_NAND_001` |
| TEST_LLD_04 | `UT_HAL_*` | `UT_HAL_NAND_001` |
| TEST_LLD_05 | `UT_RTOS_*`, `UT_LOG_*` | `UT_RTOS_001` |
| TEST_LLD_06 | `UT_FTL_*`, `UT_GC_*`, `UT_WL_*` | `UT_FTL_001` |
| System tests | `ST-NNN` | `ST-001` |
| Fault injection | `FI-NNN` | `FI-001` |

---

## 4. Terminology Glossary

The following terms must be used consistently across all documents. Do not use the listed alternatives.

| Canonical Term | Definition | Do NOT Use |
|---------------|------------|------------|
| **UPLP** | Unexpected Power Loss Protection -- enterprise SSD feature for data safety during power failure | "UPL", "power loss protection" without the UPLP acronym |
| **T10 DIF/PI** | T10 Data Integrity Field / Protection Information -- end-to-end data integrity standard | "DIF" alone (without T10 context), "PI" alone (without DIF context) |
| **DWRR** | Deficit Weighted Round Robin -- fair scheduling algorithm for multi-queue bandwidth allocation | "DRR", "WRR" when referring to the deficit-weighted variant |
| **WRR** | Weighted Round Robin -- NVMe command arbitration method (distinct from DWRR) | Acceptable in NVMe arbitration context; do not confuse with DWRR |
| **AES-XTS** | AES in XEX-based Tweaked-codebook mode with ciphertext Stealing -- the encryption mode used for data-at-rest | "AES-CBC", "AES" alone when referring to the encryption mode |
| **TCG Opal** | Trusted Computing Group Opal Security Subsystem Class -- self-encrypting drive protocol | "Opal" alone without "TCG" prefix |
| **supercapacitor** | Energy storage component for UPLP (one word, lowercase) | "super-capacitor", "super capacitor", "Super Capacitor" |
| **namespace** | NVMe logical address space (one word, lowercase) | "name space", "name-space" |
| **FTL** | Flash Translation Layer | |
| **GC** | Garbage Collection | |
| **WL** | Wear Leveling | |
| **WAF** | Write Amplification Factor | |
| **EAT** | Effective Access Time -- timing simulation engine | |
| **BBT** | Bad Block Table | |
| **L2P / P2L** | Logical-to-Physical / Physical-to-Logical mapping | |
| **OOB** | Out-of-Band (management interface) or Out-of-Band (NAND spare area), depending on context | |
| **HAL** | Hardware Abstraction Layer | |
| **RTOS** | Real-Time Operating System | |
| **WAL** | Write-Ahead Log | |
| **CWB** | Current Write Block | |
| **SQ / CQ** | Submission Queue / Completion Queue (NVMe) | |
| **AER** | Asynchronous Event Request (NVMe) | |
| **SPSC** | Single-Producer Single-Consumer (ring buffer) | |

---

## 5. Cross-Reference Format

### 5.1 Document References

When referencing another document, use the full filename:

- Correct: "See `LLD_17_POWER_LOSS_PROTECTION_EN.md`, Section 4."
- Incorrect: "See LLD_17" or "See the power loss document."

### 5.2 Requirement References

Always use the full `REQ-NNN` format:

- Correct: "This implements REQ-139 through REQ-146."
- Incorrect: "This implements requirements 139-146."

### 5.3 Section References

When referencing a section within the same document, use the section number and title:

- Correct: "See Section 3.2 (Data Structures)."
- Incorrect: "See above."

### 5.4 Test Case References

Use the full test case ID with prefix:

- Correct: "Verified by UT_FTL_001."
- Incorrect: "Verified by test 1."

---

## 6. Version Control

### 6.1 Revision History Table Format

Every document must include a Revision History table immediately after the title:

```markdown
## Revision History

| Version | Date       | Author | Description |
|---------|------------|--------|-------------|
| V1.0    | 2026-03-08 | HFSSS  | Initial release |
| V1.1    | 2026-03-23 | HFSSS  | Added enterprise features |
```

### 6.2 Version Numbering

- **Major version** (V1.0, V2.0): Significant structural changes or new major sections.
- **Minor version** (V1.1, V1.2): Content additions, corrections, or clarifications.
- Version numbers in the revision history must match the document's stated version.

### 6.3 PRD Version Alignment

- Core requirements (REQ-001 to REQ-138): PRD V1.0.
- Enterprise requirements (REQ-139 to REQ-178): PRD V2.0.
- Implementation milestones follow the roadmap versions (V1.0 through V3.0).

---

## 7. Formatting Guidelines

- Use ATX-style Markdown headers (`#`, `##`, `###`).
- Code blocks use triple backticks with language identifier (```c, ```markdown).
- Tables use GitHub-Flavored Markdown pipe syntax.
- Diagrams use ASCII art within fenced code blocks.
- Line length: no hard limit, but keep table rows readable.
- One blank line between sections; no trailing whitespace.
