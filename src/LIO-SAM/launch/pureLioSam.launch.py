import os
from ament_index_python.packages import get_package_share_directory
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, Command
from launch_ros.actions import Node


def generate_launch_description():

    share_dir = get_package_share_directory('lio_sam_op')
    parameter_file = LaunchConfiguration('params_file')
    xacro_path = os.path.join(share_dir, 'config', 'robot.urdf.xacro')
    rviz_config_file = os.path.join(share_dir, 'config', 'rviz2.rviz')

    params_declare = DeclareLaunchArgument(
        'params_file',
        default_value=os.path.join(
            share_dir, 'config', 'params.yaml'),
        description='FPath to the ROS2 parameters file to use.')

    print("urdf_file_name : {}".format(xacro_path))

    return LaunchDescription([
        params_declare,
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_imuPreintegration',
            name='lio_sam_op_imuPreintegration',
            parameters=[parameter_file],
            output='screen'
        ),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_imageProjection',
            name='lio_sam_op_imageProjection',
            parameters=[parameter_file],
            output='screen'
        ),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_featureExtraction',
            name='lio_sam_op_featureExtraction',
            parameters=[parameter_file],
            output='screen'
        ),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_mapOptimization',
            name='lio_sam_op_mapOptimization',
            parameters=[parameter_file],
            output='screen'
        ),
        Node(
            package='lio_sam_op',
            executable='lio_sam_op_transformFusion',
            name='lio_sam_op_transformFusion',
            parameters=[parameter_file],
            output='screen'
        ),
    ])