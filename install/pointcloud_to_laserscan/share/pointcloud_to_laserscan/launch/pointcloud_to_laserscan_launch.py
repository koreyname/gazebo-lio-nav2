from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument(
            name='scanner', default_value='scanner',
            description='Namespace for sample topics'
        ),
        Node(
            package='pointcloud_to_laserscan', executable='pointcloud_to_laserscan_node',
            remappings=[('cloud_in',  ['/pointcloud_out']),
                        ('scan',  ['/scan'])],
            parameters=[{
                'target_frame': 'lidar_link',
                'transform_tolerance': 0.05,
                'min_height': -0.10,
                'max_height': 0.50,
                'angle_min': -3.14159,  # -M_PI/2
                'angle_max': 3.14159,  # M_PI/2
                'angle_increment': 0.0063,  # M_PI/360.0
                'scan_time': 0.125,
                'range_min': 0.12,
                'range_max': 30.0,
                'use_inf': True,
                'inf_epsilon': 1.0
            }],
            name='pointcloud_to_laserscan'
        )
    ])
