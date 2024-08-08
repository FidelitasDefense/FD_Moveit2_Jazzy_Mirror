/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2024, Andrew Zelenak
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

#include <moveit/online_signal_smoothing/ruckig_filter.h>
#include <rclcpp/clock.hpp>
#include <rclcpp/logging.hpp>

// Disable -Wold-style-cast because all _THROTTLE macros trigger this
#pragma GCC diagnostic ignored "-Wold-style-cast"

namespace online_signal_smoothing
{
namespace
{
rclcpp::Logger getLogger()
{
  return moveit::getLogger("moveit.core.ruckig_filter_plugin");
}
inline constexpr double DEFAULT_JERK_BOUND = 300;  // rad/s^3
}  // namespace

bool RuckigFilterPlugin::initialize(rclcpp::Node::SharedPtr node, moveit::core::RobotModelConstPtr robot_model,
                                    size_t num_joints)
{
  node_ = node;
  num_joints_ = num_joints;
  robot_model_ = robot_model;
  have_initial_ruckig_output_ = false;

  // get node parameters and store in member variables
  auto param_listener = online_signal_smoothing::ParamListener(node_);
  params_ = param_listener.get_params();

  // Ruckig needs the joint vel/accel/jerk bounds.
  getVelAccelJerkBounds();
  ruckig::InputParameter<ruckig::DynamicDOFs> ruckig_input(num_joints_);
  ruckig_input.max_velocity = joint_velocity_bounds_;
  ruckig_input.max_acceleration = joint_acceleration_bounds_;
  ruckig_input.max_jerk = joint_jerk_bounds_;
  ruckig_input.current_position = std::vector<double>(num_joints_, 0.0);
  ruckig_input.current_velocity = std::vector<double>(num_joints_, 0.0);
  ruckig_input.current_acceleration = std::vector<double>(num_joints_, 0.0);
  ruckig_input_ = ruckig_input;

  ruckig_output_.emplace(ruckig::OutputParameter<ruckig::DynamicDOFs>(num_joints_));

  ruckig_.emplace(ruckig::Ruckig<ruckig::DynamicDOFs>(num_joints_, params_.update_period));

  return true;
}

bool RuckigFilterPlugin::doSmoothing(Eigen::VectorXd& positions, Eigen::VectorXd& velocities,
                                     Eigen::VectorXd& accelerations)
{
  if (have_initial_ruckig_output_ && ruckig_input_ && ruckig_output_)
  {
    ruckig_output_->pass_to_input(*ruckig_input_);
  }

  // Update Ruckig target state
  // This assumes stationary at the target (zero vel, zero accel)
  ruckig_input_->target_position = std::vector<double>(positions.data(), positions.data() + positions.size());

  // Call the Ruckig algorithm
  ruckig::Result ruckig_result = ruckig_->update(*ruckig_input_, *ruckig_output_);

  // Finished means the target state can be reached in this timestep.
  // Working means the target state can be reached but not in this timestep.
  // ErrorSynchronizationCalculation means smoothing was successful but the robot will deviate a bit from the desired
  // path.
  // See https://github.com/pantor/ruckig/blob/master/include/ruckig/input_parameter.hpp
  if (ruckig_result != ruckig::Result::Finished && ruckig_result != ruckig::Result::Working &&
      ruckig_result != ruckig::Result::ErrorSynchronizationCalculation)

  {
    RCLCPP_ERROR_STREAM(getLogger(), "Ruckig jerk-limited smoothing failed with code: " << ruckig_result);
    printRuckigState();
    // Return without modifying the position/vel/accel
    have_initial_ruckig_output_ = false;
    return true;
  }

  // Update the target state with Ruckig output
  positions = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(ruckig_output_->new_position.data(),
                                                            ruckig_output_->new_position.size());
  velocities = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(ruckig_output_->new_velocity.data(),
                                                             ruckig_output_->new_velocity.size());
  accelerations = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(ruckig_output_->new_acceleration.data(),
                                                                ruckig_output_->new_acceleration.size());
  have_initial_ruckig_output_ = true;

  return true;
}

bool RuckigFilterPlugin::reset(const Eigen::VectorXd& positions, const Eigen::VectorXd& velocities,
                               const Eigen::VectorXd& accelerations)
{
  // Initialize Ruckig
  ruckig_input_->current_position = std::vector<double>(positions.data(), positions.data() + positions.size());
  ruckig_input_->current_velocity = std::vector<double>(velocities.data(), velocities.data() + velocities.size());
  ruckig_input_->current_acceleration =
      std::vector<double>(accelerations.data(), accelerations.data() + accelerations.size());

  have_initial_ruckig_output_ = false;
  return true;
}

void RuckigFilterPlugin::getVelAccelJerkBounds()
{
  auto joint_model_group = robot_model_->getJointModelGroup(params_.planning_group_name);
  for (const auto& joint : joint_model_group->getActiveJointModels())
  {
    const auto& bound = joint->getVariableBounds(joint->getName());

    if (bound.velocity_bounded_)
    {
      // Assume symmetric limits
      joint_velocity_bounds_.push_back(bound.max_velocity_);
    }
    else
    {
      RCLCPP_ERROR(getLogger(), "No joint velocity limit defined. Exiting for safety.");
      std::exit(EXIT_FAILURE);
    }

    if (bound.acceleration_bounded_)
    {
      // Assume symmetric limits
      joint_acceleration_bounds_.push_back(bound.max_acceleration_);
    }
    else
    {
      RCLCPP_WARN(getLogger(), "No joint acceleration limit defined. Very large accelerations will be "
                               "possible.");
      joint_acceleration_bounds_.push_back(DBL_MAX);
    }

    if (bound.jerk_bounded_)
    {
      // Assume symmetric limits
      joint_jerk_bounds_.push_back(bound.max_jerk_);
    }
    else
    {
      RCLCPP_ERROR_STREAM(getLogger(), "WARNING: No joint jerk limit defined. A default jerk limit of "
                                           << DEFAULT_JERK_BOUND << " rad/s^3 has been applied.");
      joint_jerk_bounds_.push_back(DEFAULT_JERK_BOUND);
    }
  }

  assert(joint_jerk_bounds_.size() == num_joints_);
}

void RuckigFilterPlugin::printRuckigState()
{
  RCLCPP_INFO_STREAM(getLogger(), ruckig_->delta_time << "\nRuckig input:\n"
                                                      << ruckig_input_->to_string() << "\nRuckig output:\n"
                                                      << ruckig_output_->to_string());
}
}  // namespace online_signal_smoothing

#include <pluginlib/class_list_macros.hpp>
PLUGINLIB_EXPORT_CLASS(online_signal_smoothing::RuckigFilterPlugin, online_signal_smoothing::SmoothingBaseClass)
