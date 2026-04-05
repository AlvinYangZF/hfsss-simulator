#!/bin/bash

set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_nvme "nvme-list" "list"
hfsss_case_run_nvme "nvme-id-ctrl" "id-ctrl $HFSSS_GUEST_NVME_CTRL"
hfsss_case_run_nvme "nvme-smart-log" "smart-log $HFSSS_GUEST_NVME_CTRL"

hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-list.stdout" "$HFSSS_GUEST_NVME_DEV"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-list.stdout" "HFSSS0001"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-id-ctrl.stdout" "QEMU NVMe Ctrl"
hfsss_case_assert_file_contains "$HFSSS_CASE_ARTIFACT_DIR/nvme-smart-log.stdout" "media_errors"
