#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/scripts/launch_common.sh"

source_runtime
stop_project_processes
echo "本项目的建图、导航和 Gazebo 进程已停止。"
