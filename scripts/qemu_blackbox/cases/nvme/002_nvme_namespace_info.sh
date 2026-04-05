#!/bin/bash

set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_run_nvme "nvme-list-ns" "list-ns $HFSSS_GUEST_NVME_CTRL"
hfsss_case_run_nvme "nvme-id-ns" "id-ns $HFSSS_GUEST_NVME_DEV"
hfsss_case_run_nvme "nvme-list-subsys" "list-subsys"
