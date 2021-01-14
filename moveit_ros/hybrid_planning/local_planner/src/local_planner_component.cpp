/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2020, PickNik LLC
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of PickNik LLC nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Sebastian Jahr
 */

#include <moveit/local_planner/local_planner_component.h>

#include <moveit/planning_scene/planning_scene.h>
#include <moveit/robot_state/robot_state.h>

#include <moveit/robot_state/conversions.h>

#include <moveit_msgs/msg/constraints.hpp>
namespace moveit_hybrid_planning
{
const rclcpp::Logger LOGGER = rclcpp::get_logger("local_planner_component");

LocalPlannerComponent::LocalPlannerComponent(const rclcpp::NodeOptions& options)
  : Node("local_planner_component", options)
{
  state_ = moveit_hybrid_planning::LocalPlannerState::UNCONFIGURED;

  // Initialize local planner after construction
  timer_ = this->create_wall_timer(std::chrono::milliseconds(1), [this]() {
    switch (state_)
    {
      case moveit_hybrid_planning::LocalPlannerState::READY:
      {
        timer_->cancel();
        break;
      }
      case moveit_hybrid_planning::LocalPlannerState::UNCONFIGURED:
        if (this->initialize())
        {
          state_ = moveit_hybrid_planning::LocalPlannerState::READY;
        }
        else
        {
          const std::string error = "Failed to initialize global planner";
          RCLCPP_FATAL(LOGGER, error);
        }
      default:
        break;
    }
  });
}

bool LocalPlannerComponent::initialize()
{
  const auto node_ptr = shared_from_this();

  // Load planner parameter
  config_.load(node_ptr);

  // Configure planning scene monitor
  planning_scene_monitor_.reset(new planning_scene_monitor::PlanningSceneMonitor(
      node_ptr, "robot_description", tf_buffer_, "local_planner/planning_scene_monitor"));
  if (planning_scene_monitor_->getPlanningScene())
  {
    // Start state and scene monitors
    planning_scene_monitor_->startSceneMonitor();
  }
  else
  {
    const std::string error = "Unable to configure planning scene monitor";
    RCLCPP_FATAL(LOGGER, error);
    throw std::runtime_error(error);
  }

  // Load trajectory operator plugin
  try
  {
    trajectory_operator_loader_.reset(new pluginlib::ClassLoader<moveit_hybrid_planning::TrajectoryOperatorInterface>(
        "moveit_hybrid_planning", "moveit_hybrid_planning::TrajectoryOperatorInterface"));
  }
  catch (pluginlib::PluginlibException& ex)
  {
    RCLCPP_FATAL(LOGGER, "Exception while creating trajectory operator plugin loader %s", ex.what());
  }
  try
  {
    trajectory_operator_instance_ =
        trajectory_operator_loader_->createUniqueInstance(config_.trajectory_operator_plugin_name);
    if (!trajectory_operator_instance_->initialize(node_ptr, planning_scene_monitor_->getRobotModel(),
                                                   "panda_arm"))  // TODO add default group param
      throw std::runtime_error("Unable to initialize trajectory operator plugin");
    RCLCPP_INFO(LOGGER, "Using trajectory operator interface '%s'", config_.trajectory_operator_plugin_name.c_str());
  }
  catch (pluginlib::PluginlibException& ex)
  {
    RCLCPP_ERROR(LOGGER, "Exception while loading trajectory operator '%s': %s",
                 config_.trajectory_operator_plugin_name.c_str(), ex.what());
  }

  // Load constraint solver
  try
  {
    solver_plugin_loader_.reset(new pluginlib::ClassLoader<moveit_hybrid_planning::ConstraintSolverInterface>(
        "moveit_hybrid_planning", "moveit_hybrid_planning::ConstraintSolverInterface"));
  }
  catch (pluginlib::PluginlibException& ex)
  {
    RCLCPP_FATAL(LOGGER, "Exception while creating constraint solver plugin loader %s", ex.what());
  }
  try
  {
    constraint_solver_instance_ = solver_plugin_loader_->createUniqueInstance(config_.solver_plugin_name);
    if (!constraint_solver_instance_->initialize(node_ptr))
      throw std::runtime_error("Unable to initialize constraint solver plugin");
    RCLCPP_INFO(LOGGER, "Using constraint solver interface '%s'", config_.solver_plugin_name.c_str());
  }
  catch (pluginlib::PluginlibException& ex)
  {
    RCLCPP_ERROR(LOGGER, "Exception while loading constraint solver '%s': %s", config_.solver_plugin_name.c_str(),
                 ex.what());
  }

  // Initialize local planning request action server
  using namespace std::placeholders;
  local_planning_request_server_ = rclcpp_action::create_server<moveit_msgs::action::LocalPlanner>(
      this->get_node_base_interface(), this->get_node_clock_interface(), this->get_node_logging_interface(),
      this->get_node_waitables_interface(), "local_planning_action",
      [](const rclcpp_action::GoalUUID& /*unused*/,
         std::shared_ptr<const moveit_msgs::action::LocalPlanner::Goal> /*unused*/) {
        RCLCPP_INFO(LOGGER, "Received local planning goal request");
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
      },
      [](const std::shared_ptr<rclcpp_action::ServerGoalHandle<moveit_msgs::action::LocalPlanner>>& /*unused*/) {
        RCLCPP_INFO(LOGGER, "Received request to cancel local planning goal");
        return rclcpp_action::CancelResponse::ACCEPT;
      },
      [this](std::shared_ptr<rclcpp_action::ServerGoalHandle<moveit_msgs::action::LocalPlanner>> goal_handle) {
        local_planning_goal_handle_ = std::move(goal_handle);
        // Start local planning loop when an action request is received
        timer_ = this->create_wall_timer(std::chrono::milliseconds(config_.cycle_time),
                                         std::bind(&LocalPlannerComponent::executePlanningLoopRun, this));
      });

  // Initialize global trajectory listener
  global_solution_subscriber_ = create_subscription<moveit_msgs::msg::MotionPlanResponse>(
      config_.global_solution_topic, 1, [this](const moveit_msgs::msg::MotionPlanResponse::SharedPtr msg) {
        // Add received trajectory to internal reference trajectory
        robot_trajectory::RobotTrajectory new_trajectory(planning_scene_monitor_->getRobotModel(), msg->group_name);
        moveit::core::RobotState start_state(planning_scene_monitor_->getRobotModel());
        moveit::core::robotStateMsgToRobotState(msg->trajectory_start, start_state);
        new_trajectory.setRobotTrajectoryMsg(start_state, msg->trajectory);
        trajectory_operator_instance_->addTrajectorySegment(new_trajectory);

        // Update local planner state
        state_ = moveit_hybrid_planning::LocalPlannerState::LOCAL_PLANNING_ACTIVE;
      });

  // Initialize local solution publisher
  local_solution_publisher_ =
      this->create_publisher<trajectory_msgs::msg::JointTrajectory>(config_.local_solution_topic, 1);

  state_ = moveit_hybrid_planning::LocalPlannerState::READY;
  return true;
}

void LocalPlannerComponent::executePlanningLoopRun()
{
  auto result = std::make_shared<moveit_msgs::action::LocalPlanner::Result>();

  // Do different things depending on the planner's internal state
  switch (state_)
  {
    // If READY start waiting for trajectory
    case moveit_hybrid_planning::LocalPlannerState::READY:
    {
      state_ = moveit_hybrid_planning::LocalPlannerState::AWAIT_GLOBAL_TRAJECTORY;
      break;
    }
    // Wait for global solution to be published
    case moveit_hybrid_planning::LocalPlannerState::AWAIT_GLOBAL_TRAJECTORY:
      // Do nothing
      break;
    // Notify action client that local planning failed
    case moveit_hybrid_planning::LocalPlannerState::ABORT:
    {
      local_planning_goal_handle_->abort(result);
      timer_->cancel();
      RCLCPP_ERROR(LOGGER, "Local planner somehow failed :(");

      // TODO add proper reset function
      state_ = moveit_hybrid_planning::LocalPlannerState::READY;
      break;
    }
    // If the planner received an action request and a global solution it starts to plan locally
    case moveit_hybrid_planning::LocalPlannerState::LOCAL_PLANNING_ACTIVE:
    {
      // Clone current planning scene
      planning_scene_monitor_->updateFrameTransforms();
      planning_scene_monitor_->lockSceneRead();  // LOCK planning scene
      planning_scene::PlanningScenePtr planning_scene = planning_scene::PlanningScene::clone(
          planning_scene_monitor_->getPlanningScene());  // TODO remove expensive planning scene cloning
      planning_scene_monitor_->unlockSceneRead();        // UNLOCK planning scene

      // Get current state
      auto current_robot_state = planning_scene->getCurrentStateNonConst();

      // Check if the global goal is reached
      if (trajectory_operator_instance_->getTrajectoryProgress(current_robot_state) == 1.0)
      {
        local_planning_goal_handle_->succeed(result);
        state_ = moveit_hybrid_planning::LocalPlannerState::READY;
        timer_->cancel();
        break;
      }

      // Get and solve local planning problem
      std::vector<moveit_msgs::msg::Constraints> current_goal_constraint =
          trajectory_operator_instance_->getLocalProblem(current_robot_state);
      const auto goal = local_planning_goal_handle_->get_goal();
      auto local_feedback = std::make_shared<moveit_msgs::action::LocalPlanner::Feedback>();
      trajectory_msgs::msg::JointTrajectory local_solution = constraint_solver_instance_->solve(
          current_goal_constraint, goal->local_constraints, planning_scene, local_feedback);

      if (!local_feedback->feedback.empty())
      {
        local_planning_goal_handle_->publish_feedback(local_feedback);
      }

      // Publish control command
      local_solution_publisher_->publish(local_solution);
      break;
    }
    default:
    {
      local_planning_goal_handle_->abort(result);
      timer_->cancel();
      RCLCPP_ERROR(LOGGER, "Local planner somehow failed :(");
      state_ = moveit_hybrid_planning::LocalPlannerState::READY;
      break;
    }
  }
};
}  // namespace moveit_hybrid_planning

// Register the component with class_loader
#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(moveit_hybrid_planning::LocalPlannerComponent)
