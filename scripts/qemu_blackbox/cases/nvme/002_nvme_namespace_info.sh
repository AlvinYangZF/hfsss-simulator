#!/bin/bash

set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_nvme "nvme-list-ns" "list-ns $HFSSS_GUEST_NVME_CTRL"
hfsss_case_run_nvme "nvme-id-ns" "id-ns $HFSSS_GUEST_NVME_DEV"
hfsss_case_run_nvme "nvme-list-subsys" "list-subsys"

# nvme-cli prints active namespaces as bracketed rows like "[   0]:0x1".
# Match the concrete nsid=1 row terminator rather than a loose "0x1" token,
# which could false-positive on values like 0x10 or 0x11.
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-list-ns.stdout" "]:0x1$"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-id-ns.stdout" "nsze"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-id-ns.stdout" "ncap"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-list-subsys.stdout" "HFSSS0001"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-list-subsys.stdout" "$(basename "$HFSSS_GUEST_NVME_CTRL")"
