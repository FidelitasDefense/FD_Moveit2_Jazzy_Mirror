/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2019, PickNik Inc.
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
 *   * Neither the name of PickNik Inc. nor the names of its
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

/* Author: Henning Kayser */

#include <stdexcept>

#include <moveit/moveit_cpp/planning_component.h>
#include <moveit/kinematic_constraints/utils.h>
#include <moveit/planning_pipeline/planning_pipeline.h>
#include <moveit/planning_scene_monitor/planning_scene_monitor.h>
#include <moveit/robot_state/conversions.h>
#include <thread>

namespace moveit_cpp
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("moveit.ros_planning_interface.planning_component");

PlanningComponent::PlanningComponent(const std::string& group_name, const MoveItCppPtr& moveit_cpp)
  : node_(moveit_cpp->getNode()), moveit_cpp_(moveit_cpp), group_name_(group_name)
{
  joint_model_group_ = moveit_cpp_->getRobotModel()->getJointModelGroup(group_name);
  if (!joint_model_group_)
  {
    std::string error = "Could not find joint model group '" + group_name + "'.";
    RCLCPP_FATAL_STREAM(LOGGER, error);
    throw std::runtime_error(error);
  }
  planning_pipeline_names_ = moveit_cpp_->getPlanningPipelineNames(group_name);
  plan_request_parameters_.load(node_);
  RCLCPP_DEBUG_STREAM(
      LOGGER, "Default plan request parameters loaded with --"
                  << " planning_pipeline: " << plan_request_parameters_.planning_pipeline << ","
                  << " planner_id: " << plan_request_parameters_.planner_id << ","
                  << " planning_time: " << plan_request_parameters_.planning_time << ","
                  << " planning_attempts: " << plan_request_parameters_.planning_attempts << ","
                  << " max_velocity_scaling_factor: " << plan_request_parameters_.max_velocity_scaling_factor << ","
                  << " max_acceleration_scaling_factor: " << plan_request_parameters_.max_acceleration_scaling_factor);
}

PlanningComponent::PlanningComponent(const std::string& group_name, const rclcpp::Node::SharedPtr& node)
  : PlanningComponent(group_name, std::make_shared<MoveItCpp>(node))
{
  joint_model_group_ = moveit_cpp_->getRobotModel()->getJointModelGroup(group_name);
  if (!joint_model_group_)
  {
    std::string error = "Could not find joint model group '" + group_name + "'.";
    RCLCPP_FATAL_STREAM(LOGGER, error);
    throw std::runtime_error(error);
  }
  planning_pipeline_names_ = moveit_cpp_->getPlanningPipelineNames(group_name);
}

const std::vector<std::string> PlanningComponent::getNamedTargetStates()
{
  if (joint_model_group_)
  {
    return joint_model_group_->getDefaultStateNames();
  }
  else
  {
    RCLCPP_WARN(LOGGER, "Unable to find joint group with name '%s'.", group_name_.c_str());
  }

  std::vector<std::string> empty;
  return empty;
}

const std::string& PlanningComponent::getPlanningGroupName() const
{
  return group_name_;
}

bool PlanningComponent::setPathConstraints(const moveit_msgs::msg::Constraints& path_constraints)
{
  current_path_constraints_ = path_constraints;
  return true;
}

bool PlanningComponent::setTrajectoryConstraints(const moveit_msgs::msg::TrajectoryConstraints& trajectory_constraints)
{
  current_trajectory_constraints_ = trajectory_constraints;
  return true;
}

planning_interface::MotionPlanResponse PlanningComponent::plan(const PlanRequestParameters& parameters,
                                                               const bool update_last_solution)
{
  auto plan_solution = planning_interface::MotionPlanResponse();

  // check if joint_model_group exists
  if (!joint_model_group_)
  {
    RCLCPP_ERROR(LOGGER, "Failed to retrieve joint model group for name '%s'.", group_name_.c_str());
    plan_solution.error_code_ = moveit::core::MoveItErrorCode::INVALID_GROUP_NAME;
    if (update_last_solution)
    {
      last_plan_solution_ = plan_solution;
    }
    return plan_solution;
  }

  // Clone current planning scene
  planning_scene_monitor::PlanningSceneMonitorPtr planning_scene_monitor = moveit_cpp_->getPlanningSceneMonitor();
  planning_scene_monitor->updateFrameTransforms();
  const planning_scene::PlanningScenePtr planning_scene = [planning_scene_monitor] {
    planning_scene_monitor::LockedPlanningSceneRO ls(planning_scene_monitor);
    return planning_scene::PlanningScene::clone(ls);
  }();
  planning_scene_monitor.reset();  // release this pointer

  // Init MotionPlanRequest
  ::planning_interface::MotionPlanRequest req;
  req.group_name = group_name_;
  req.planner_id = parameters.planner_id;
  req.num_planning_attempts = std::max(1, parameters.planning_attempts);
  req.allowed_planning_time = parameters.planning_time;
  req.max_velocity_scaling_factor = parameters.max_velocity_scaling_factor;
  req.max_acceleration_scaling_factor = parameters.max_acceleration_scaling_factor;
  if (workspace_parameters_set_)
    req.workspace_parameters = workspace_parameters_;

  // Set start state
  moveit::core::RobotStatePtr start_state = considered_start_state_;
  if (!start_state)
    start_state = moveit_cpp_->getCurrentState();
  start_state->update();
  moveit::core::robotStateToRobotStateMsg(*start_state, req.start_state);
  planning_scene->setCurrentState(*start_state);

  // Set goal constraints
  if (current_goal_constraints_.empty())
  {
    RCLCPP_ERROR(LOGGER, "No goal constraints set for planning request");
    plan_solution.error_code_ = moveit::core::MoveItErrorCode::INVALID_GOAL_CONSTRAINTS;
    if (update_last_solution)
    {
      last_plan_solution_ = plan_solution;
    }
    return plan_solution;
  }
  req.goal_constraints = current_goal_constraints_;
  // Set path constraints
  req.path_constraints = current_path_constraints_;
  // Set trajectory constraints
  req.trajectory_constraints = current_trajectory_constraints_;

  // Run planning attempt
  ::planning_interface::MotionPlanResponse res;
  if (planning_pipeline_names_.find(parameters.planning_pipeline) == planning_pipeline_names_.end())
  {
    RCLCPP_ERROR(LOGGER, "No planning pipeline available for name '%s'", parameters.planning_pipeline.c_str());
    plan_solution.error_code_ = moveit::core::MoveItErrorCode::FAILURE;
    if (update_last_solution)
    {
      last_plan_solution_ = plan_solution;
    }
    return plan_solution;
  }
  const planning_pipeline::PlanningPipelinePtr pipeline =
      moveit_cpp_->getPlanningPipelines().at(parameters.planning_pipeline);
  pipeline->generatePlan(planning_scene, req, res);

  plan_solution.error_code_ = res.error_code_;
  if (res.error_code_.val != res.error_code_.SUCCESS)
  {
    RCLCPP_ERROR(LOGGER, "Could not compute plan successfully");
    if (update_last_solution)
    {
      last_plan_solution_ = plan_solution;
    }
    return plan_solution;
  }
  plan_solution.trajectory_ = res.trajectory_;
  plan_solution.planning_time_ = res.planning_time_;
  plan_solution.start_state_ = req.start_state;
  plan_solution.error_code_ = res.error_code_.val;

  // TODO(henningkayser): Visualize trajectory
  // std::vector<const moveit::core::LinkModel*> eef_links;
  // if (joint_model_group->getEndEffectorTips(eef_links))
  //{
  //  for (const auto& eef_link : eef_links)
  //  {
  //    RCLCPP_INFO_STREAM("Publishing trajectory for end effector " << eef_link->getName());
  //    visual_tools_->publishTrajectoryLine(last_solution_trajectory_, eef_link);
  //    visual_tools_->publishTrajectoryPath(last_solution_trajectory_, false);
  //    visual_tools_->publishRobotState(last_solution_trajectory_->getLastWayPoint(), rviz_visual_tools::TRANSLUCENT);
  //  }
  //}

  if (update_last_solution)
  {
    last_plan_solution_ = plan_solution;
  }
  return plan_solution;
}

planning_interface::MotionPlanResponse PlanningComponent::plan(const MultiPipelinePlanRequestParameters& parameters,
                                                               SolutionCallbackFunction solution_selection_callback,
                                                               StoppingCriterionFunction stopping_criterion_callback)
{
  // Create solutions container
  PlanningComponent::PlanSolutions planning_solutions{ parameters.multi_plan_request_parameters.size() };
  std::vector<std::thread> planning_threads;
  planning_threads.reserve(parameters.multi_plan_request_parameters.size());

  // Print a warning if more parallel planning problems than available concurrent threads are defined. If
  // std::thread::hardware_concurrency() is not defined, the command returns 0 so the check does not work
  auto const hardware_concurrency = std::thread::hardware_concurrency();
  if (parameters.multi_plan_request_parameters.size() > hardware_concurrency && hardware_concurrency != 0)
  {
    RCLCPP_WARN(
        LOGGER,
        "More parallel planning problems defined ('%ld') than possible to solve concurrently with the hardware ('%d')",
        parameters.multi_plan_request_parameters.size(), hardware_concurrency);
  }

  // Launch planning threads
  for (auto const& plan_request_parameter : parameters.multi_plan_request_parameters)
  {
    auto planning_thread = std::thread([&]() {
      auto plan_solution = planning_interface::MotionPlanResponse();
      try
      {
        plan_solution = plan(plan_request_parameter, false);
      }
      catch (const std::exception& e)
      {
        RCLCPP_ERROR_STREAM(LOGGER, "Planning pipeline '" << plan_request_parameter.planning_pipeline.c_str()
                                                          << "' threw exception '" << e.what() << "'");
        auto plan_solution = planning_interface::MotionPlanResponse();
        plan_solution.error_code_ = moveit::core::MoveItErrorCode::FAILURE;
      }
      planning_solutions.pushBack(plan_solution);

      if (stopping_criterion_callback != nullptr)
      {
        if (stopping_criterion_callback(planning_solutions, parameters))
        {
          // Terminate planning pipelines
          RCLCPP_ERROR_STREAM(LOGGER, "Stopping criterion met: Terminating planning pipelines that are still active");
          for (auto const& plan_request_parameter : parameters.multi_plan_request_parameters)
          {
            moveit_cpp_->terminatePlanningPipeline(plan_request_parameter.planning_pipeline);
          }
        }
      }
    });
    planning_threads.push_back(std::move(planning_thread));
  }

  // Wait for threads to finish
  for (auto& planning_thread : planning_threads)
  {
    if (planning_thread.joinable())
    {
      planning_thread.join();
    }
  }

  // Return best solution determined by user defined callback (Default: Shortest path)
  return solution_selection_callback(planning_solutions.getSolutions());
}

planning_interface::MotionPlanResponse PlanningComponent::plan()
{
  return plan(plan_request_parameters_);
}

bool PlanningComponent::setStartState(const moveit::core::RobotState& start_state)
{
  considered_start_state_ = std::make_shared<moveit::core::RobotState>(start_state);
  return true;
}

moveit::core::RobotStatePtr PlanningComponent::getStartState()
{
  if (considered_start_state_)
    return considered_start_state_;
  else
  {
    moveit::core::RobotStatePtr s;
    moveit_cpp_->getCurrentState(s, 1.0);
    return s;
  }
}

bool PlanningComponent::setStartState(const std::string& start_state_name)
{
  const auto& named_targets = getNamedTargetStates();
  if (std::find(named_targets.begin(), named_targets.end(), start_state_name) == named_targets.end())
  {
    RCLCPP_ERROR(LOGGER, "No predefined joint state found for target name '%s'", start_state_name.c_str());
    return false;
  }
  moveit::core::RobotState start_state(moveit_cpp_->getRobotModel());
  start_state.setToDefaultValues(joint_model_group_, start_state_name);
  return setStartState(start_state);
}

void PlanningComponent::setStartStateToCurrentState()
{
  considered_start_state_.reset();
}

std::map<std::string, double> PlanningComponent::getNamedTargetStateValues(const std::string& name)
{
  // TODO(henningkayser): verify result
  std::map<std::string, double> positions;
  joint_model_group_->getVariableDefaultPositions(name, positions);
  return positions;
}

void PlanningComponent::setWorkspace(double minx, double miny, double minz, double maxx, double maxy, double maxz)
{
  workspace_parameters_.header.frame_id = moveit_cpp_->getRobotModel()->getModelFrame();
  workspace_parameters_.header.stamp = node_->now();
  workspace_parameters_.min_corner.x = minx;
  workspace_parameters_.min_corner.y = miny;
  workspace_parameters_.min_corner.z = minz;
  workspace_parameters_.max_corner.x = maxx;
  workspace_parameters_.max_corner.y = maxy;
  workspace_parameters_.max_corner.z = maxz;
  workspace_parameters_set_ = true;
}

void PlanningComponent::unsetWorkspace()
{
  workspace_parameters_set_ = false;
}

bool PlanningComponent::setGoal(const std::vector<moveit_msgs::msg::Constraints>& goal_constraints)
{
  current_goal_constraints_ = goal_constraints;
  return true;
}

bool PlanningComponent::setGoal(const moveit::core::RobotState& goal_state)
{
  current_goal_constraints_ = { kinematic_constraints::constructGoalConstraints(goal_state, joint_model_group_) };
  return true;
}

bool PlanningComponent::setGoal(const geometry_msgs::msg::PoseStamped& goal_pose, const std::string& link_name)
{
  current_goal_constraints_ = { kinematic_constraints::constructGoalConstraints(link_name, goal_pose) };
  return true;
}

bool PlanningComponent::setGoal(const std::string& goal_state_name)
{
  const auto& named_targets = getNamedTargetStates();
  if (std::find(named_targets.begin(), named_targets.end(), goal_state_name) == named_targets.end())
  {
    RCLCPP_ERROR(LOGGER, "No predefined joint state found for target name '%s'", goal_state_name.c_str());
    return false;
  }
  moveit::core::RobotState goal_state(moveit_cpp_->getRobotModel());
  goal_state.setToDefaultValues(joint_model_group_, goal_state_name);
  return setGoal(goal_state);
}

bool PlanningComponent::execute(bool blocking)
{
  if (!last_plan_solution_)
  {
    RCLCPP_ERROR(LOGGER, "There is no successful plan to execute");
    return false;
  }

  // TODO(henningkayser): parameterize timestamps if required
  // trajectory_processing::TimeOptimalTrajectoryGeneration totg;
  // if (!totg.computeTimeStamps(*last_solution_trajectory_, max_velocity_scaling_factor_,
  // max_acceleration_scaling_factor_))
  //{
  //  RCLCPP_ERROR("Failed to parameterize trajectory");
  //  return false;
  //}
  return moveit_cpp_->execute(group_name_, last_plan_solution_.trajectory_, blocking);
}

planning_interface::MotionPlanResponse const& PlanningComponent::getLastMotionPlanResponse()
{
  return last_plan_solution_;
}

planning_interface::MotionPlanResponse
getShortestSolution(std::vector<planning_interface::MotionPlanResponse> const& solutions)
{
  // Find trajectory with minimal path
  auto const shortest_trajectory = std::min_element(solutions.begin(), solutions.end(),
                                                    [](planning_interface::MotionPlanResponse const& solution_a,
                                                       planning_interface::MotionPlanResponse const& solution_b) {
                                                      // If both solutions were successful, check which path is shorter
                                                      if (solution_a && solution_b)
                                                      {
                                                        return robot_trajectory::path_length(*solution_a.trajectory_) <
                                                               robot_trajectory::path_length(*solution_b.trajectory_);
                                                      }
                                                      // If only solution a is successful, return a
                                                      else if (solution_a)
                                                      {
                                                        return true;
                                                      }
                                                      // Else return solution b, either because it is successful or not
                                                      return false;
                                                    });
  if (shortest_trajectory->trajectory_ != nullptr)
  {
    RCLCPP_INFO(LOGGER, "Chosen solution with shortest path length: '%f'",
                robot_trajectory::path_length(*shortest_trajectory->trajectory_));
  }
  else
  {
    RCLCPP_INFO_STREAM(LOGGER, "Could not determine shortest path");
  }
  return *shortest_trajectory;
}
}  // namespace moveit_cpp
