# 相对 Gitee 原项目的优化与改动

## 1. 对比对象

本文比较：

- Gitee 原项目：<https://gitee.com/dedm/lio-sam_simulation_navigation.git>
- 对比提交：`7f4ff3cbbc080fef93f95d7837dbc6fdfd79f098`
- 提交日期：2024-04-27
- 当前工作区：在该项目基础上完成修复和扩展后的版本

本次比较的是完整仿真导航项目，不是单独与
`TixiaoShan/LIO-SAM` 官方仓库比较。

## 2. 总体结论

当前项目相对 Gitee 原项目做了实质性优化，主要集中在：

1. 建图和导航模式分离。
2. Gazebo 点云字段修复。
3. LIO-SAM 参数与当前世界、雷达和 IMU 匹配。
4. 全局点云地图显示与回环拼接恢复。
5. TF 树和机器人模型显示修复。
6. Nav2 从 Humble 参数迁移到当前 Foxy 环境。
7. 小车生成和重复启动可靠性提升。
8. 新增三维 PCD 和二维占据栅格地图保存流程。
9. 新增完整的启动检查、停止脚本和排障文档。

最准确的项目描述是：

> 在 Gitee 原始仿真项目基础上，修复点云、TF、启动时序和 ROS 版本兼容问题，
> 将原本混合运行的建图导航流程重构为可独立使用的 LIO-SAM 建图与 Nav2 导航系统，
> 并补充地图保存和三维转二维能力。

当前修改以工程稳定性和场景适配为主。虽然地图表现和长期运行稳定性得到改善，
但没有通过 ATE、RPE 等指标与原项目进行定量对比，因此不应直接宣称定位精度
提高了某个具体百分比。

## 3. 启动架构优化

### 3.1 原项目

原项目只有一个硬编码的 `nav.sh`：

```bash
ros2 launch pointcloud_to_laserscan pointcloud_to_laserscan_launch.py
ros2 launch pointcloud_downsampling pointcloud_downsampling.launch.py
ros2 launch lio_sam run.launch.py
ros2 launch robot_navigation2 navigation2.launch.py
ros2 launch robot_bringup bringup.launch.py
```

该脚本存在以下问题：

- 同时启动 LIO-SAM 建图和 AMCL/Nav2 导航。
- 多套定位和 TF 逻辑同时运行。
- 启动顺序缺少依赖检查。
- 工作区路径硬编码为 `/home/hdm/...`。
- 不检查 Gazebo、小车、传感器和 `/scan` 是否真正启动。
- 没有统一停止和残留进程清理。

### 3.2 当前项目

新增：

```text
mapping.sh
navigation.sh
stop.sh
scripts/launch_common.sh
```

建图：

```bash
./mapping.sh
```

只启动：

- Gazebo
- 小车和传感器
- 点云字段转换
- `lio_sam_op`
- 建图 RViz

导航：

```bash
./navigation.sh
```

只启动：

- Gazebo
- 点云转 LaserScan
- map_server
- AMCL
- Nav2
- 导航 RViz

优化效果：

- 消除建图与导航同时运行造成的 TF 冲突。
- 减少无用节点和资源占用。
- 脚本不再依赖固定用户目录。
- 各模式职责明确，更容易调试。

## 4. 启动可靠性优化

### 4.1 原项目问题

原项目按固定顺序打开终端，没有等待前置组件就绪，因此容易出现：

```text
Unable to start server[bind: Address already in use]
Waiting for service /spawn_entity
```

也会出现 Gazebo 世界已经打开，但小车没有加载的情况。

### 4.2 当前改动

启动前清理本项目残留的：

- `gzserver`
- Gazebo launch
- LIO-SAM
- pointcloud_to_laserscan
- Nav2
- RViz
- 长时间残留的 ROS 查询进程

启动过程中等待：

1. `/spawn_entity` 服务。
2. `/imu` 话题。
3. `/pointclouds` 话题。
4. `/get_model_list` 返回 `robot`。
5. 导航模式的 `/scan`。
6. Nav2 生命周期管理服务。

任何关键步骤失败时，不继续启动后续模块。

优化效果：

- 降低 Gazebo 11345 端口占用问题。
- 提高小车模型生成成功率。
- 避免传感器未启动时直接启动 LIO-SAM 或 Nav2。
- 避免重复 RViz 和过期 TF。

## 5. Gazebo 小车生成优化

原项目直接启动 `spawn_entity.py`，容易与 Gazebo factory 服务形成时序竞争。

当前 `bringup.launch.py`：

- 使用 `TimerAction` 延迟 2 秒生成小车。
- 生成高度设置为 `z=0.07`，降低模型嵌入地面的概率。
- `robot_description` 只生成一次并复用。
- 新增 `publish_odom` 参数，支持建图与导航使用不同的里程计策略。

同时在 URDF 中为车轮关节增加：

```xml
<dynamics damping="0.2" friction="0.1"/>
```

并增加车轮接触相关限制，改善静止和运动时的物理稳定性。

## 6. 点云预处理修复

### 6.1 原项目的问题

原 `point_deal.cpp`：

- 订阅 `/pointcloud`，与当前 Gazebo 实际输出 `/pointclouds` 不一致。
- 使用普通队列 QoS，不适合传感器数据。
- `ring` 按 `[-90°, 90°]` 粗略映射，但实际雷达垂直视场约为 30°。
- `time` 字段没有赋值。
- 没有明确设置点云 `width`、`height` 和 `is_dense`。
- 输出 frame 直接继承输入，可能与 LIO-SAM 的 `lidar_link` 配置不一致。

其中未初始化的 `time` 会直接影响 LIO-SAM 点云去畸变。

### 6.2 当前改动

当前处理链路：

```text
/pointclouds -> point_deal -> /pointcloud_out
```

修改内容：

- 使用 `rclcpp::SensorDataQoS()`。
- 按当前 16 线雷达的约 `-15°` 至 `15°` 垂直视场计算 `ring`。
- 将 `ring` 限制在 `0` 至 `15`。
- 根据水平角度和 8 Hz 扫描周期生成每个点的相对 `time`。
- 预留点云内存，减少动态扩容。
- 设置有组织信息和稠密状态。
- 输出 frame 统一为 `lidar_link`。

优化效果：

- 点云字段符合 LIO-SAM 的 Velodyne 输入要求。
- 修复去畸变使用未初始化时间的问题。
- 减少点云行号越界。
- 统一点云和 TF 坐标系。

## 7. 移除建图链路中的重复降采样

原项目同时启动：

- `pointcloud_downsampling`
- LIO-SAM 内部体素滤波

当前建图流程不再启动额外的 `pointcloud_downsampling`，而是让
`point_deal` 直接把 `/pointcloud_out` 送入 LIO-SAM。

优化效果：

- 缩短点云话题链。
- 避免重复降采样损失特征。
- 减少一个进程和一次点云复制。
- 方便检查 LIO-SAM 的真实输入。

因此当前版本已经删除 `pointcloud_downsampling` 包，避免保留无效依赖和误用入口。

## 8. LIO-SAM 参数优化

配置：

```text
src/LIO-SAM/config/params.yaml
```

### 8.1 雷达参数与 Gazebo 匹配

Gitee 原项目：

```yaml
N_SCAN: 16
Horizon_SCAN: 1800
lidarMinRange: 0.5
lidarMaxRange: 100.0
```

当前项目：

```yaml
N_SCAN: 16
Horizon_SCAN: 640
lidarMinRange: 0.2
lidarMaxRange: 30.0
```

原因：

- 当前世界文件的雷达水平采样数是 640，不是 1800。
- 世界尺寸约为几十米，不需要 100 米量程。
- 室内小场景需要保留近距离墙面。

这是重要修复。`Horizon_SCAN` 不匹配会影响点云投影列号、特征分布和地图质量。

### 8.2 IMU 参数

Gitee 原项目：

```yaml
imuAccNoise: 0.01
imuGyrNoise: 0.001
imuAccBiasN: 0.001
imuGyrBiasN: 0.0001
imuRPYWeight: 0.00
```

当前项目：

```yaml
imuAccNoise: 5.0e-03
imuGyrNoise: 1.0e-04
imuAccBiasN: 1.0e-05
imuGyrBiasN: 1.0e-06
imuRPYWeight: 0.01
```

优化目标：

- 与当前 Gazebo IMU 噪声模型更接近。
- 降低偏差随机游走导致的长期漂移。
- 小幅使用 IMU Roll/Pitch 约束。

这些参数只适合当前仿真模型，不能直接用于真实 IMU。

### 8.3 特征阈值

Gitee 原项目：

```yaml
edgeThreshold: 0.1
edgeFeatureMinValidNum: 4
```

当前项目：

```yaml
edgeThreshold: 1.0
edgeFeatureMinValidNum: 10
```

Gazebo 几何结构规则，过低的边缘阈值会选入较多弱响应点。当前参数提高边缘特征
质量要求，并要求更多有效特征后再执行优化。

### 8.4 平面小车约束

Gitee 原项目基本不限制三维运动：

```yaml
z_tollerance: 1000.0
rotation_tollerance: 1000.0
```

当前项目：

```yaml
z_tollerance: 0.3
rotation_tollerance: 0.35
```

优化效果：

- 限制平地小车不合理的 Z 轴漂移。
- 限制 Roll/Pitch 持续累积。
- 降低长时间运行后地图倾斜和墙面重影。

该设置不适合坡道和明显三维运动场景。

### 8.5 处理频率和 CPU

Gitee 原项目：

```yaml
numberOfCores: 2
mappingProcessInterval: 0.05
```

当前项目：

```yaml
numberOfCores: 4
mappingProcessInterval: 0.1
```

当前雷达约为 8 Hz，`0.1` 秒的处理间隔更接近传感器频率，避免无意义地配置高于
输入频率的地图处理循环，同时允许使用更多 CPU 核心。

### 8.6 关键帧和局部地图

Gitee 原项目：

```yaml
surroundingkeyframeAddingDistThreshold: 1.0
surroundingkeyframeAddingAngleThreshold: 0.2
surroundingKeyframeDensity: 1.0
surroundingKeyframeSearchRadius: 8.0
```

当前项目：

```yaml
surroundingkeyframeAddingDistThreshold: 0.4
surroundingkeyframeAddingAngleThreshold: 0.15
surroundingKeyframeDensity: 0.4
surroundingKeyframeSearchRadius: 20.0
```

优化效果：

- 小型室内世界中保存更密集的关键帧。
- 车辆移动距离较短时也能积累有效地图。
- 局部匹配可使用更完整的周围环境。

代价是关键帧和计算量增加。

### 8.7 启用回环检测

Gitee 原项目：

```yaml
loopClosureEnableFlag: false
```

当前项目：

```yaml
loopClosureEnableFlag: true
historyKeyframeSearchRadius: 15.0
historyKeyframeFitnessScore: 0.2
```

优化效果：

- 小车重新经过旧区域时可建立回环约束。
- 后端能够更新历史关键帧位姿。
- 降低长时间运行后的累计漂移。

这也是原项目“地图运行一段时间后变乱”的重要改进之一。

### 8.8 全局地图显示

Gitee 原项目：

```yaml
globalMapVisualizationPoseDensity: 10.0
globalMapVisualizationLeafSize: 1.0
```

当前项目：

```yaml
globalMapVisualizationPoseDensity: 0.4
globalMapVisualizationLeafSize: 0.1
```

原参数对当前小型世界降采样过强，容易让 RViz 看起来只剩局部点云。

当前设置保留更密集的关键帧和点云，因此全局地图显示更完整。

这主要是显示和地图细节优化，不等同于后端精度提升。

## 9. LIO-SAM 包与 TF 结构调整

Gitee 原项目使用包名：

```text
lio_sam
```

当前版本使用：

```text
lio_sam_op
```

并引入了经过重构的 LIO-SAM 代码：

- 话题和 frame 参数更明确。
- 新增 `imuFrame`、`imuOdomTopic`、`lidarOdomTopic` 和 `saveMapSrv`。
- 成员变量命名统一。
- 增加较完整的中文注释。
- 将 `transformFusion` 拆分为独立节点。

当前 `transformFusion` 使用稳定的雷达里程计并结合
`lidar_link -> base_link` 外参发布建图模型 TF，降低 RViz 中模型和坐标轴明显抖动。

该部分同时包含代码重构和运行行为调整。LIO-SAM 的特征提取、IMU 预积分、
scan-to-map、因子图和回环主体仍沿用原有框架。

## 10. 建图全局坐标系修复

Gitee 原项目同时配置：

```text
odometryFrame: odom
mapFrame: map
```

当前建图配置统一为：

```yaml
odomFrame: map
```

优化效果：

- LIO-SAM 全局地图、轨迹和注册点云使用统一的 `map` frame。
- RViz Fixed Frame 更明确。
- 避免用户看到 `odom` 后误认为没有全局 `map`。
- 减少不必要的固定 `map -> odom` 变换。

## 11. RViz 显示优化

当前 `src/LIO-SAM/config/rviz2.rviz` 相对原项目进行了重新配置：

- Fixed Frame 使用建图全局坐标。
- 增加并整理全局地图、注册点云和原始雷达点云显示。
- 原始点云订阅 `/pointcloud_out`。
- 关闭建图阶段无用的导航路径显示。
- 使用 RobotModel 显示小车。

优化效果：

- 建图阶段界面只突出 LIO-SAM 相关信息。
- 可以区分局部点云和全局拼接地图。
- 减少坐标轴、Marker 和导航路径造成的视觉干扰。

## 12. Gazebo 导航 TF 修复

Gitee 原项目在底盘插件中固定关闭：

```xml
<publish_odom>false</publish_odom>
<publish_odom_tf>false</publish_odom_tf>
```

当前改为可配置，并明确：

```xml
<publish_odom>$(arg publish_odom)</publish_odom>
<publish_odom_tf>$(arg publish_odom)</publish_odom_tf>
<odometry_frame>odom</odometry_frame>
<robot_base_frame>base_link</robot_base_frame>
```

使用策略：

- 建图：关闭 Gazebo odom TF。
- 导航：开启 Gazebo odom TF。

最终导航 TF 链：

```text
map -> odom -> base_link -> lidar_link
```

修复了原项目可能出现的：

```text
Could not find a connection between 'odom' and 'base_link'
Tf has two or more unconnected trees
Message Filter dropping message: frame 'lidar_link'
```

## 13. PointCloud 转 LaserScan 参数优化

Gitee 原项目：

```yaml
transform_tolerance: 0.01
min_height: -0.20
max_height: 3.5
scan_time: 0.2333
range_max: 10.0
```

当前项目：

```yaml
transform_tolerance: 0.05
min_height: -0.10
max_height: 0.50
scan_time: 0.125
range_max: 30.0
```

优化原因：

- `0.125` 秒对应当前 8 Hz 雷达。
- 高度范围只保留适合二维导航的障碍物截面。
- 30 米量程与 Gazebo 雷达和地图范围一致。
- 更合理的 TF 容忍时间降低消息过滤。

## 14. ROS 版本兼容修复

Gitee README 声明使用 Ubuntu 22.04 和 ROS 2 Humble，其 `robot.yaml` 也包含
Humble 风格配置，例如：

- `nav2_behaviors`
- `behavior_server`
- `smoother_server`
- Humble 行为树插件
- `nav2_amcl::DifferentialMotionModel`

当前实际环境是 ROS 2 Foxy，因此当前项目完成了 Foxy 适配：

- `recoveries_server` 替代 Humble behavior server 配置。
- 使用 Foxy 可用的 BT 插件列表。
- 补充 `nav2_initial_pose_received_condition_bt_node`。
- 修正 Foxy 的 goal checker 参数名。
- `robot_model_type` 使用 Foxy 支持的值。
- `use_sim_time` 默认开启。

优化效果：

- Nav2 节点能够完成生命周期配置并进入 `active`。
- `/navigate_to_pose`、`/compute_path_to_pose` 和 `/follow_path` 可用。

## 15. AMCL 初始化优化

原项目没有为当前 Gazebo 固定出生点配置可靠的 AMCL 初始位姿。

当前项目在 `robot.yaml` 中增加：

```yaml
set_initial_pose: true
always_reset_initial_pose: true
initial_pose:
  x: 0.0
  y: 0.0
  z: 0.0
  yaw: 0.0
```

优化效果：

- 启动导航后自动建立 `map -> odom`。
- 不必每次手动点击 `2D Pose Estimate`。
- 避免一次性 `/initialpose` 消息受启动时序影响。

如果以后修改 Gazebo 出生位置，必须同步修改该参数。

## 16. Costmap 配置优化

Gitee 原项目的局部 costmap 同时加载：

- static layer
- obstacle layer
- voxel layer
- inflation layer

其中局部 costmap 使用 `odom` frame，静态层却依赖全局 `map`，在 AMCL 初始化前
容易持续报告 `map` 不存在。

当前局部 costmap 只保留：

```yaml
plugins: ["voxel_layer", "inflation_layer"]
```

全局 costmap 保留：

```yaml
plugins: ["static_layer", "obstacle_layer", "inflation_layer"]
```

优化效果：

- 消除局部 costmap 对全局静态地图的不合理依赖。
- 避免重复使用同一个 `/scan` 障碍层。
- 降低启动阶段错误和额外计算。

## 17. 地图保存功能新增

Gitee 原项目没有提供顶层地图保存命令。

当前新增：

```text
save_map.sh
save_2d_map.sh
tools/pcd_to_occupancy_grid.py
```

### 17.1 保存三维点云

```bash
./save_map.sh
```

输出：

```text
maps/lio_sam_map/GlobalMap.pcd
```

脚本会检查 LIO-SAM 保存服务是否存在，并验证保存结果。

### 17.2 生成二维导航地图

```bash
./save_2d_map.sh
```

处理流程：

```text
GlobalMap.pcd
-> 高度裁剪
-> XY 投影
-> 栅格统计
-> 噪点过滤
-> 障碍膨胀
-> my_map.pgm + my_map.yaml
```

输出：

```text
src/robot_navigation2/maps/my_map.pgm
src/robot_navigation2/maps/my_map.yaml
```

这是当前版本相对 Gitee 原项目最明确的新增能力之一，实现了：

```text
LIO-SAM 三维建图 -> Nav2 二维导航
```

的自动转换。

## 18. 地图文件更新

当前 `my_map.pgm` 和 `my_map.yaml` 已由当前 Gazebo 世界重新生成。

地图参数：

```yaml
resolution: 0.05
origin: [-10.300000, -6.000000, 0.0]
```

相对原项目，地图原点和图像内容已经与当前建图结果同步，而不是继续使用仓库内
旧的静态地图。

## 19. 文档和使用流程增强

Gitee 原 README 只提供：

```bash
colcon build
./nav.sh
```

当前项目增加：

- 建图使用流程。
- 导航使用流程。
- 三维地图保存。
- 二维地图生成。
- TF、话题和 Nav2 生命周期验证命令。
- 常见错误、原因和解决记录。
- 修改参数时的注意事项。

相关文档：

```text
docs/项目整合问题与解决记录.md
docs/相对Gitee原项目的优化与改动.md
```

## 20. 哪些属于优化，哪些属于适配

### 可以明确称为优化

- 建图和导航分离。
- 点云 `ring/time` 修复。
- 雷达参数与世界文件匹配。
- 平面运动约束。
- 回环检测启用。
- 全局地图显示密度调整。
- TF 树修复。
- 小车生成检测。
- 启动残留进程清理。
- 三维转二维地图流程。
- 局部 costmap 简化。

### 更适合称为版本适配

- Humble Nav2 参数迁移到 Foxy。
- BT 插件列表调整。
- behavior server 改为 recovery server。
- AMCL motion model 参数形式调整。

### 更适合称为重构

- 包名改为 `lio_sam_op`。
- `transformFusion` 拆分。
- 成员变量命名统一。
- 增加中文注释。
- 公共 shell 启动逻辑抽取。

## 21. 当前仍需定量验证的内容

若要正式证明当前版本在精度和性能上优于 Gitee 原项目，建议增加：

### 轨迹精度

- ATE RMSE
- RPE 平移误差
- RPE 旋转误差
- 闭环前后终点误差

### 地图质量

- 墙面重影宽度
- 地图完整率
- 错误回环数量
- 长时间运行漂移

### 性能

- 平均 CPU
- 峰值内存
- 单帧处理时间
- 关键帧数量

### 稳定性

- 连续启动成功率
- 小车生成成功率
- Nav2 完整激活成功率
- 运行 30 分钟后的 TF 和地图状态

## 22. 推荐项目描述

可以使用：

> 本项目基于 Gitee 的 `dedm/lio-sam_simulation_navigation`，修复了原项目中
> 建图与导航混合启动、Gazebo 点云时间字段缺失、雷达参数不匹配、TF 树断开、
> Nav2 版本不兼容和小车生成不稳定等问题。进一步完成了 LIO-SAM 参数调优、
> 回环与全局地图显示优化，并新增三维点云地图保存、二维占据栅格生成以及
> 建图/导航独立启动流程。

不建议在没有定量实验前使用：

> 当前版本全面提升了 LIO-SAM 的定位精度。

## 23. 最终评价

相对 Gitee 原项目，当前版本已经从“一次性启动全部节点的演示工程”改造成：

```text
可独立建图
可保存三维地图
可转换二维地图
可独立定位导航
可检查启动状态
可重复稳定运行
```

的完整仿真 SLAM 与导航工程。
