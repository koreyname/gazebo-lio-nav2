#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/scripts/launch_common.sh"

source_runtime

resolution="${1:-0.10}"
destination="${2:-$SCRIPT_DIR/maps/lio_sam_map}"

if ! ros2 service list | grep -Fxq "/lio_sam/save_map"; then
  echo "LIO-SAM 建图节点未运行，请先执行: ./mapping.sh" >&2
  exit 1
fi

mkdir -p "$(dirname "$destination")"

echo "正在保存三维点云地图..."
echo "分辨率: ${resolution} m"
echo "目录: $destination"

response="$(
  ros2 service call /lio_sam/save_map lio_sam_op/srv/SaveMap \
    "{resolution: ${resolution}, destination: '$destination'}"
)"

printf '%s\n' "$response"

if ! grep -q "success=True" <<<"$response"; then
  echo "地图保存失败。" >&2
  exit 1
fi

echo "地图保存完成: $destination/GlobalMap.pcd"
