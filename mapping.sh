#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/scripts/launch_common.sh"

source_runtime
prepare_mode
start_simulation false

launch_in_terminal "lio_sam_mapping" \
  "ros2 launch lio_sam_op run.launch.py use_sim_time:=true"
