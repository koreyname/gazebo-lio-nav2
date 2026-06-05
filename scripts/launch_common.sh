#!/usr/bin/env bash

set -euo pipefail

WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROS_DISTRO="${ROS_DISTRO:-foxy}"
ROS_SETUP="/opt/ros/${ROS_DISTRO}/setup.bash"

if [[ ! -f "$ROS_SETUP" ]]; then
  echo "找不到 ROS 2 环境: $ROS_SETUP" >&2
  exit 1
fi

if [[ ! -f "$WORKSPACE_DIR/install/setup.bash" ]]; then
  echo "工作区尚未编译，请先运行: colcon build" >&2
  exit 1
fi

source_runtime() {
  set +u
  export PATH="/usr/bin:$PATH"
  source "$ROS_SETUP"
  source "$WORKSPACE_DIR/install/setup.bash"
  if [[ -f /usr/share/gazebo/setup.sh ]]; then
    source /usr/share/gazebo/setup.sh
  fi
  set -u
}

launch_in_terminal() {
  local title="$1"
  local command="$2"

  gnome-terminal --title="$title" -- bash -lc "
    cd '$WORKSPACE_DIR'
    export PATH='/usr/bin':\"\$PATH\"
    source '$ROS_SETUP'
    source '$WORKSPACE_DIR/install/setup.bash'
    [[ -f /usr/share/gazebo/setup.sh ]] && source /usr/share/gazebo/setup.sh
    $command
    exec bash
  "
}

stop_project_processes() {
  local patterns=(
    "$WORKSPACE_DIR/install/robot_bringup"
    "$WORKSPACE_DIR/install/lio_sam"
    "$WORKSPACE_DIR/install/lio_sam_op"
    "$WORKSPACE_DIR/install/pointcloud_to_laserscan"
    "$WORKSPACE_DIR/install/robot_navigation2"
    "$WORKSPACE_DIR/install/robot_bringup/share/robot_bringup/world/my_world.world"
    "ros2 launch robot_bringup bringup.launch.py"
    "ros2 launch lio_sam run.launch.py"
    "ros2 launch lio_sam_op run.launch.py"
    "ros2 launch pointcloud_to_laserscan pointcloud_to_laserscan_launch.py"
    "ros2 launch robot_navigation2 navigation2.launch.py"
    "ros2 topic hz /map"
    "ros2 param get /lifecycle_manager_navigation autostart"
    "/opt/ros/$ROS_DISTRO/lib/rviz2/rviz2 -d $WORKSPACE_DIR/install/lio_sam_op"
    "/opt/ros/$ROS_DISTRO/lib/rviz2/rviz2 -d /opt/ros/$ROS_DISTRO/share/nav2_bringup"
    "/opt/ros/$ROS_DISTRO/lib/rviz2/rviz2"
  )
  local pattern

  for pattern in "${patterns[@]}"; do
    pkill -TERM -f "$pattern" 2>/dev/null || true
  done

  sleep 2
}

wait_for_service() {
  local service_name="$1"
  local timeout_seconds="$2"
  local elapsed=0

  while (( elapsed < timeout_seconds )); do
    if ros2 service list 2>/dev/null | grep -Fxq "$service_name"; then
      return 0
    fi
    sleep 1
    ((elapsed += 1))
  done

  echo "等待 ROS 2 服务超时: $service_name" >&2
  return 1
}

wait_for_topic() {
  local topic_name="$1"
  local timeout_seconds="$2"
  local elapsed=0

  while (( elapsed < timeout_seconds )); do
    if ros2 topic list 2>/dev/null | grep -Fxq "$topic_name"; then
      return 0
    fi
    sleep 1
    ((elapsed += 1))
  done

  echo "等待 ROS 2 话题超时: $topic_name" >&2
  return 1
}

wait_for_model() {
  local model_name="$1"
  local timeout_seconds="$2"
  local elapsed=0
  local response

  while (( elapsed < timeout_seconds )); do
    response="$(timeout 3s ros2 service call /get_model_list gazebo_msgs/srv/GetModelList '{}' 2>/dev/null || true)"
    if grep -q "'$model_name'" <<<"$response"; then
      return 0
    fi
    sleep 1
    ((elapsed += 1))
  done

  echo "等待 Gazebo 模型超时: $model_name" >&2
  return 1
}

prepare_mode() {
  echo "正在停止本项目残留的建图、导航和 Gazebo 进程..."
  stop_project_processes
}

start_simulation() {
  local publish_odom="$1"

  launch_in_terminal "robot_bringup" \
    "ros2 launch robot_bringup bringup.launch.py publish_odom:=$publish_odom"

  if ! wait_for_service "/spawn_entity" 20; then
    echo "Gazebo 未成功启动，已停止后续节点。" >&2
    echo "请检查 11345 端口是否被其他 Gazebo 工程占用。" >&2
    return 1
  fi

  if ! wait_for_topic "/imu" 30 || ! wait_for_topic "/pointclouds" 30; then
    echo "机器人或传感器未成功生成，已停止后续节点。" >&2
    return 1
  fi

  if ! wait_for_model "robot" 30; then
    echo "Gazebo 中未发现小车模型，已停止后续节点。" >&2
    return 1
  fi

  sleep 1
}
