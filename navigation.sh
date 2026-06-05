#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/scripts/launch_common.sh"

source_runtime
prepare_mode
start_simulation true

launch_in_terminal "pointcloud_to_laserscan" \
  "ros2 launch pointcloud_to_laserscan pointcloud_to_laserscan_launch.py"

if ! wait_for_topic "/scan" 20; then
  echo "激光扫描未成功生成，已停止导航启动。" >&2
  exit 1
fi

launch_in_terminal "robot_navigation2" \
  "ros2 launch robot_navigation2 navigation2.launch.py"

if ! wait_for_service "/lifecycle_manager_navigation/is_active" 30; then
  echo "Nav2 未成功启动。" >&2
  exit 1
fi

echo "导航已启动，AMCL 使用 Gazebo 出生点 (0, 0, 0) 初始化。"
