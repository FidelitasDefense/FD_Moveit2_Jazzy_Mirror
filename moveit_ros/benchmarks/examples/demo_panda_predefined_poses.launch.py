import os
from launch import LaunchDescription
from launch_ros.actions import Node
from ament_index_python.packages import get_package_share_directory

from moveit_utils.launch_test_utils import load_file, load_yaml


def generate_launch_description():

    moveit_ros_benchmarks_yaml_file_name = get_package_share_directory('moveit_ros_benchmarks') + "/demo_panda_predefined_poses.yaml"

    # Component yaml files are grouped in separate namespaces
    robot_description_config = load_file('moveit_resources', 'panda_description/urdf/panda.urdf')
    robot_description = {'robot_description' : robot_description_config}

    robot_description_semantic_config = load_file('moveit_resources', 'panda_moveit_config/config/panda.srdf')
    robot_description_semantic = {'robot_description_semantic' : robot_description_semantic_config}

    kinematics_yaml = load_yaml('moveit_resources', 'panda_moveit_config/config/kinematics.yaml')
    robot_description_kinematics = { 'robot_description_kinematics' : kinematics_yaml }

    ompl_planning_pipeline_config = { 'ompl' : {
        'planning_plugin' : 'ompl_interface/OMPLPlanner',
        'request_adapters' : """default_planner_request_adapters/AddTimeOptimalParameterization default_planner_request_adapters/FixWorkspaceBounds default_planner_request_adapters/FixStartStateBounds default_planner_request_adapters/FixStartStateCollision default_planner_request_adapters/FixStartStatePathConstraints""" ,
        'start_state_max_bounds_error' : 0.1 } }
    ompl_planning_yaml = load_yaml('moveit_resources', 'panda_moveit_config/config/ompl_planning.yaml')
    ompl_planning_pipeline_config['ompl'].update(ompl_planning_yaml)

    # moveit_ros_benchmark demo executable
    moveit_ros_benchmarks_node = Node(node_name='moveit_run_benchmark',
                                      package='moveit_ros_benchmarks',
                                      prefix='xterm -e gdb --ex=run --args',
                                      node_executable='moveit_combine_predefined_poses_benchmark',
                                      output='screen',
                                      parameters=[moveit_ros_benchmarks_yaml_file_name,
                                                  robot_description,
                                                  robot_description_semantic,
                                                  robot_description_kinematics,
                                                  ompl_planning_pipeline_config])

    return LaunchDescription([moveit_ros_benchmarks_node])

