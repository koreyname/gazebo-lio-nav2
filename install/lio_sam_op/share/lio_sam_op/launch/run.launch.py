import os

from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    share_dir = get_package_share_directory('lio_sam_op')
    parameter_file = LaunchConfiguration('params_file')
    use_sim_time = LaunchConfiguration('use_sim_time')
    rviz_config_file = os.path.join(share_dir, 'config', 'rviz2.rviz')

    common_parameters = [
        parameter_file,
        {'use_sim_time': use_sim_time},
    ]

    return LaunchDescription([
        DeclareLaunchArgument(
            'params_file',
            default_value=os.path.join(share_dir, 'config', 'params.yaml'),
            description='Full path to the LIO-SAM parameter file.'),
        DeclareLaunchArgument(
            'use_sim_time',
            default_value='true',
            description='Use the Gazebo simulation clock.'),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_imuPreintegration',
            name='lio_sam_op_imuPreintegration',
            parameters=common_parameters,
            output='screen'),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_imageProjection',
            name='lio_sam_op_imageProjection',
            parameters=common_parameters,
            output='screen'),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_featureExtraction',
            name='lio_sam_op_featureExtraction',
            parameters=common_parameters,
            output='screen'),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_mapOptimization',
            name='lio_sam_op_mapOptimization',
            parameters=common_parameters,
            output='screen'),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_transformFusion',
            name='lio_sam_op_transformFusion',
            parameters=common_parameters,
            output='screen'),
        Node(
            package='rviz2',
            executable='rviz2',
            name='lio_sam_rviz',
            arguments=['-d', rviz_config_file],
            parameters=[{'use_sim_time': use_sim_time}],
            output='screen'),
    ])
