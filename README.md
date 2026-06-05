# lio-sam_simulation_navigation

#### 介绍
本项目在仿真环境中使用lio_sam算法进行定位，从而使用navigation进行机器人导航。

注：本项目使用的lio_sam算法对原版的做了略微的修改，以适配仿真环境。原版lio_sam算法链接：https://github.com/TixiaoShan/LIO-SAM/tree/ros2

当前工作区实际运行版本：ROS 2 Foxy + Gazebo Classic。

项目整合、问题原因和完整解决过程见：

[项目整合问题与解决记录](docs/项目整合问题与解决记录.md)

当前版本相对 Gitee 原始项目的代码、参数和工程能力差异见：

[相对 Gitee 原项目的优化与改动](docs/相对Gitee原项目的优化与改动.md)

![输入图片说明](https://foruda.gitee.com/images/1713445774866583926/f19afb5d_14318961.png "2078cdad88fcf4d177fdfa00131707e7.png")

使用说明

1. 克隆

git clone https://gitee.com/dedm/lio-sam_simulation_navigation.git

2. 编译

colcon build 

3. 建图

```bash
./mapping.sh
```

该模式启动 Gazebo、点云预处理和 LIO-SAM，不启动 Nav2。

4. 导航

```bash
./navigation.sh
```

该模式加载 `src/robot_navigation2/maps/my_map.yaml`，启动 Gazebo、
LaserScan 转换、AMCL 和 Nav2，不启动 LIO-SAM 建图。

旧命令 `./nav.sh` 仍可使用，但现在仅作为 `./navigation.sh` 的兼容别名。

建图和导航脚本会在启动前清理本项目残留的 Gazebo/ROS 进程。也可以手动停止：

```bash
./stop.sh
```

