#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/scripts/launch_common.sh"
source_runtime

resolution="${1:-0.05}"
pcd_dir="$SCRIPT_DIR/maps/lio_sam_map"
map_base="$SCRIPT_DIR/src/robot_navigation2/maps/my_map"

"$SCRIPT_DIR/save_map.sh" "$resolution" "$pcd_dir"

/usr/bin/python3 "$SCRIPT_DIR/tools/pcd_to_occupancy_grid.py" \
  "$pcd_dir/GlobalMap.pcd" \
  "$map_base" \
  --resolution "$resolution" \
  --min-z 0.15 \
  --max-z 2.0 \
  --padding 0.5 \
  --min-points 2 \
  --dilate 1

echo "正在更新 robot_navigation2 安装目录..."
colcon build --packages-select robot_navigation2 --symlink-install

echo "二维导航地图保存完成: ${map_base}.yaml"
