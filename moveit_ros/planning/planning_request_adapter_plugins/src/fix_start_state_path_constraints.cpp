/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2013, Ioan A. Sucan
 *  Copyright (c) 2012, Willow Garage, Inc.
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
 *   * Neither the name of Willow Garage nor the names of its
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

/* Author: Ioan Sucan */

#include <moveit/planning_request_adapter/planning_request_adapter.h>
#include <moveit/robot_state/conversions.h>
#include <class_loader/class_loader.hpp>
#include <moveit/trajectory_processing/trajectory_tools.h>
#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/node.hpp>

namespace default_planner_request_adapters
{
rclcpp::Logger LOGGER = rclcpp::get_logger("moveit_ros.fix_start_state_path_constraints");

class FixStartStatePathConstraints : public planning_request_adapter::PlanningRequestAdapter
{
public:
  FixStartStatePathConstraints() : planning_request_adapter::PlanningRequestAdapter()
  {
  }

  void initialize(const rclcpp::Node::SharedPtr& /* node */, const std::string& /* parameter_namespace */) override
  {
  }

  std::string getDescription() const override
  {
    return "Fix Start State Path Constraints";
  }

  bool adaptAndPlan(const PlannerFn& planner, const planning_scene::PlanningSceneConstPtr& planning_scene,
                    const planning_interface::MotionPlanRequest& req, planning_interface::MotionPlanResponse& res,
                    std::vector<std::size_t>& added_path_index) const override
  {
    RCLCPP_DEBUG(LOGGER, "Running '%s'", getDescription().c_str());

    // get the specified start state
    moveit::core::RobotState start_state = planning_scene->getCurrentState();
    moveit::core::robotStateMsgToRobotState(planning_scene->getTransforms(), req.start_state, start_state);

    // if the start state is otherwise valid but does not meet path constraints
    if (planning_scene->isStateValid(start_state, req.group_name) &&
        !planning_scene->isStateValid(start_state, req.path_constraints, req.group_name))
    {
      RCLCPP_INFO(LOGGER, "Path constraints not satisfied for start state...");
      planning_scene->isStateValid(start_state, req.path_constraints, req.group_name, true);
      RCLCPP_INFO(LOGGER, "Planning to path constraints...");

      planning_interface::MotionPlanRequest req2 = req;
      req2.goal_constraints.resize(1);
      req2.goal_constraints[0] = req.path_constraints;
      req2.path_constraints = moveit_msgs::msg::Constraints();
      planning_interface::MotionPlanResponse res2;
      // we call the planner for this additional request, but we do not want to include potential
      // index information from that call
      std::vector<std::size_t> added_path_index_temp;
      added_path_index_temp.swap(added_path_index);
      moveit::core::MoveItErrorCode solved1 = planner(planning_scene, req2, res2);
      added_path_index_temp.swap(added_path_index);

      moveit::core::MoveItErrorCode solved2(moveit_msgs::msg::MoveItErrorCodes::FAILURE);
      if (bool(solved1))
      {
        planning_interface::MotionPlanRequest req3 = req;
        RCLCPP_INFO(LOGGER, "The start state was modified to match path constraints. Now resuming the original "
                            "planning request.");

        // extract the last state of the computed motion plan and set it as the new start state
        moveit::core::robotStateToRobotStateMsg(res2.trajectory_->getLastWayPoint(), req3.start_state);
        solved2 = planner(planning_scene, req3, res);
        res.planning_time_ += res2.planning_time_;

        if (bool(solved2))
        {
          // since we add a prefix, we need to correct any existing index positions
          for (std::size_t& added_index : added_path_index)
            added_index += res2.trajectory_->getWayPointCount();

          // we mark the fact we insert a prefix path (we specify the index position we just added)
          for (std::size_t i = 0; i < res2.trajectory_->getWayPointCount(); ++i)
            added_path_index.push_back(i);

          // we need to append the solution paths.
          res2.trajectory_->append(*res.trajectory_, 0.0);
          res2.trajectory_->swap(*res.trajectory_);
          return true;
        }
      }

      if (!bool(solved1) || !bool(solved2))
      {
        RCLCPP_WARN(LOGGER, "Unable to meet path constraints at the start.");
        res.error_code_.val = moveit_msgs::msg::MoveItErrorCodes::START_STATE_VIOLATES_PATH_CONSTRAINTS;
        return false;
      }
    }

    RCLCPP_DEBUG(LOGGER, "Path constraints are OK. Continuing without `fix_start_state_path_constraints`.");
    moveit::core::MoveItErrorCode moveit_code = planner(planning_scene, req, res);
    return bool(moveit_code);
  }
};
}  // namespace default_planner_request_adapters

CLASS_LOADER_REGISTER_CLASS(default_planner_request_adapters::FixStartStatePathConstraints,
                            planning_request_adapter::PlanningRequestAdapter)
