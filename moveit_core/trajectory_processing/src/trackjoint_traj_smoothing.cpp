/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2021, PickNik Robotics
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/

/* Author: Jack Center, Wyatt Rees, Andy Zelenak */

#include <algorithm>
#include <cmath>
#include <Eigen/Geometry>
#include <limits>
#include <moveit/online_signal_smoothing/butterworth_filter.h>
#include <moveit/trajectory_processing/trackjoint_traj_smoothing.h>
#include <rclcpp/rclcpp.hpp>
#include <trackjoint/error_codes.h>
#include <trackjoint/joint_trajectory.h>
#include <vector>

namespace trajectory_processing
{
namespace
{
const rclcpp::Logger LOGGER = rclcpp::get_logger("moveit_trajectory_processing.trackjoint_traj_smoothing");
constexpr double DEFAULT_TRACKJOINT_TIMESTEP = 0.001;
constexpr double DEFAULT_MAX_VELOCITY = 5;                    // rad/s
constexpr double DEFAULT_MAX_ACCELERATION = 10;               // rad/s^2
constexpr double DEFAULT_MAX_JERK = 20;                       // rad/s^3
constexpr double DEFAULT_WAYPOINT_POSITION_TOLERANCE = 1e-5;  // rad
constexpr double LOWPASS_FILTER_COEFFICIENT = 20.0;           // The minimum feasible filter coefficient is 1.0
}  // namespace

bool TrackJointSmoothing::applySmoothing(robot_trajectory::RobotTrajectory& reference_trajectory,
                                         const double max_velocity_scaling_factor,
                                         const double max_acceleration_scaling_factor)
{
  const moveit::core::JointModelGroup* group = reference_trajectory.getGroup();
  if (!group)
  {
    RCLCPP_ERROR(LOGGER, "It looks like the planner did not set the group the plan was computed for");
    return false;
  }

  const size_t num_waypoints = reference_trajectory.getWayPointCount();
  if (num_waypoints < 2)
  {
    RCLCPP_ERROR(LOGGER, "Trajectory does not have enough points to smooth with TrackJoint.");
    return false;
  }

  robot_trajectory::RobotTrajectory outgoing_trajectory = reference_trajectory;
  // Clear the waypoints
  outgoing_trajectory.clear();
  // ... but keep the first wayopint
  outgoing_trajectory.addSuffixWayPoint(reference_trajectory.getWayPoint(0),
                                        reference_trajectory.getWayPointDurationFromPrevious(0));

  const size_t num_dof = group->getVariableCount();

  // Look up JointGroup variables
  const std::vector<int>& joint_group_indices = group->getVariableIndexList();

  // This lib does not work properly when angles wrap around, so we need to unwind the path first
  reference_trajectory.unwind();

  // Save the reference trajectory to datafile, for analysis
  // TODO(andyz): delete when done testing
  saveRobotTrajectoryToCSV("/home/andy/Downloads/TrackJoint/input_", reference_trajectory, num_dof, joint_group_indices);

  // Current state
  std::vector<trackjoint::KinematicState> current_joint_states(num_dof);
  // Goal state
  std::vector<trackjoint::KinematicState> goal_joint_states(num_dof);
  // Kinematic limits
  std::vector<trackjoint::Limits> limits(num_dof);
  setTrackJointLimits(group, num_dof, max_velocity_scaling_factor, max_acceleration_scaling_factor, limits);

  // Pre-allocate
  double desired_duration = DEFAULT_TRACKJOINT_TIMESTEP;
  double max_duration = 0.1;
  // Initialize the smoothing object
  trackjoint::TrajectoryGenerator traj_gen(num_dof, DEFAULT_TRACKJOINT_TIMESTEP, desired_duration, max_duration,
                                           current_joint_states, goal_joint_states, limits,
                                           DEFAULT_WAYPOINT_POSITION_TOLERANCE, false /* high speed mode */);
  std::vector<trackjoint::JointTrajectory> trackjoint_output(num_dof);
  trackjoint::ErrorCodeEnum error_code;

  // Initial state
  setTrackJointState(0, reference_trajectory, num_dof, joint_group_indices, current_joint_states);

  // Do smoothing
  for (size_t waypoint_idx = 0; waypoint_idx < num_waypoints - 1; ++waypoint_idx)
  {
    setTrackJointState(waypoint_idx + 1, reference_trajectory, num_dof, joint_group_indices, goal_joint_states);

    desired_duration = reference_trajectory.getWayPointDurationFromPrevious(waypoint_idx + 1);
    max_duration = 100 * desired_duration;

    traj_gen.reset(DEFAULT_TRACKJOINT_TIMESTEP, desired_duration, max_duration, current_joint_states, goal_joint_states,
                   limits, DEFAULT_WAYPOINT_POSITION_TOLERANCE, false /* high speed mode */);
    error_code = traj_gen.inputChecking(current_joint_states, goal_joint_states, limits, DEFAULT_TRACKJOINT_TIMESTEP);
    if (error_code != trackjoint::ErrorCodeEnum::NO_ERROR)
    {
      RCLCPP_ERROR_STREAM(LOGGER, "Invalid input to TrackJoint smoothing algorithm. Error code: "
                                      << trackjoint::ERROR_CODE_MAP.at(error_code));
      return false;
    }

    error_code = traj_gen.generateTrajectories(&trackjoint_output);
    if (error_code != trackjoint::ErrorCodeEnum::NO_ERROR)
    {
      RCLCPP_ERROR_STREAM(LOGGER, "TrackJoint trajectory smoothing failed. Error code: "
                                      << trackjoint::ERROR_CODE_MAP.at(error_code));
      return false;
    }

    // Save final output to data file, for analysis
    // TODO(andyz): delete when done testing
    traj_gen.saveTrajectoriesToFile(trackjoint_output, "/home/andy/Downloads/TrackJoint/", true /* append */);

    addTrackJointOutpointToRobotTrajectory(reference_trajectory, num_dof, joint_group_indices, trackjoint_output,
                                           outgoing_trajectory);

    // Update current_joint_states from the previous output of TrackJoint
    trackjoint::KinematicState joint_state;
    size_t last_trackjoint_index = trackjoint_output.at(0).positions.size() - 1;
    for (size_t joint = 0; joint < num_dof; ++joint)
    {
      joint_state.position = trackjoint_output.at(joint).positions(last_trackjoint_index);
      joint_state.velocity = trackjoint_output.at(joint).velocities(last_trackjoint_index);
      joint_state.acceleration = trackjoint_output.at(joint).accelerations(last_trackjoint_index);
      current_joint_states[joint] = joint_state;
    }
  }

  // Iteratively smooth the trackjoint output with a low-pass filter to ensure smoothness between waypoints.
  // A very small numerical mismatch between waypoint positions (like 1e-4) can cause a large jerk spike.
  // This increases the filter coefficient for each joint until jerk limits are satisfied.
  // TODO(andyz): as of now, it only applies one filter coefficient
  if (!doIterativeLowPassFilter(num_dof, joint_group_indices, limits, outgoing_trajectory))
  {
    // This should never happen
    RCLCPP_ERROR_STREAM(LOGGER, "Iterative smoothing with a lowpass filter failed.");
    return false;
  }

  RCLCPP_INFO_STREAM(LOGGER, "TrackJoint input waypoint count: " << reference_trajectory.getWayPointCount());
  reference_trajectory = outgoing_trajectory;
  RCLCPP_INFO_STREAM(LOGGER, "TrackJoint smoothed waypoint count: " << outgoing_trajectory.getWayPointCount());
  return true;
}

bool TrackJointSmoothing::doIterativeLowPassFilter(const size_t num_dof, const std::vector<int>& joint_group_indices,
                                                   const std::vector<trackjoint::Limits>& limits,
                                                   robot_trajectory::RobotTrajectory& trajectory)
{
  size_t num_failures = 0;
  // For each joint
  for (size_t joint_idx = 0; joint_idx < num_dof; ++joint_idx)
  {
    online_signal_smoothing::ButterworthFilter joint_filter(LOWPASS_FILTER_COEFFICIENT);

    // Initialize joint position
    joint_filter.reset(trajectory.getFirstWayPointPtr()->getVariablePosition(joint_group_indices.at(joint_idx)));

    double max_position_discontinuity =
        limits.at(joint_idx).velocity_limit * DEFAULT_TRACKJOINT_TIMESTEP +
        0.5 * limits.at(joint_idx).acceleration_limit * DEFAULT_TRACKJOINT_TIMESTEP * DEFAULT_TRACKJOINT_TIMESTEP +
        0.16667 * limits.at(joint_idx).jerk_limit * pow(DEFAULT_TRACKJOINT_TIMESTEP, 3);

    // Step through and filter each waypoint
    for (size_t waypoint_idx = 1; waypoint_idx < trajectory.getWayPointCount(); ++waypoint_idx)
    {
      auto current_waypoint = trajectory.getWayPointPtr(waypoint_idx);
      auto prev_waypoint = trajectory.getWayPointPtr(waypoint_idx - 1);
      double filtered_position =
          joint_filter.filter(current_waypoint->getVariablePosition(joint_group_indices.at(joint_idx)));
      // Overwrite the previous value with the filtered value
      current_waypoint->setVariablePosition(joint_group_indices.at(joint_idx), filtered_position);

      // Check for a position discontinuity. Increase filter coefficient if needed.
      double prev_velocity = prev_waypoint->getVariableVelocity(joint_group_indices.at(joint_idx));
      double prev_acceleration = prev_waypoint->getVariableAcceleration(joint_group_indices.at(joint_idx));
      double timestep = trajectory.getWayPointDurationFromPrevious(waypoint_idx);
      double present_discontinuity =
          filtered_position - prev_waypoint->getVariablePosition(joint_group_indices.at(joint_idx));
      if (std::fabs(present_discontinuity) > std::fabs(max_position_discontinuity))
      {
        RCLCPP_ERROR_STREAM(LOGGER, "Unacceptable discontinuity detected!");
        RCLCPP_ERROR_STREAM(LOGGER, "Max discontinuity: " << max_position_discontinuity);
        RCLCPP_ERROR_STREAM(LOGGER, "present_discontinuity: " << present_discontinuity);
        RCLCPP_WARN_STREAM(LOGGER, "timestep: " << timestep);
        RCLCPP_WARN_STREAM(LOGGER, "prev velocity: " << prev_velocity);
        RCLCPP_WARN_STREAM(LOGGER, "prev acceleration: " << prev_acceleration);
        // TODO(andyz): increase filter coefficient and try again
        ++num_failures;
      }
    }
    // TODO(andyz): update velocity and acceleration data after updating positions
  }
  RCLCPP_ERROR_STREAM(LOGGER, "Num failures: " << num_failures);

  return true;
}

void TrackJointSmoothing::saveRobotTrajectoryToCSV(const std::string& base_filepath,
                                                   const robot_trajectory::RobotTrajectory& trajectory,
                                                   const size_t num_dof, const std::vector<int>& joint_group_indices)
{
  std::ofstream output_file;
  std::string output_path;

  for (size_t joint = 0; joint < num_dof; ++joint)
  {
    output_path = base_filepath + std::to_string(joint + 1) + ".csv";
    output_file.open(output_path, std::ofstream::out);

    for (size_t waypoint_idx = 0; waypoint_idx < trajectory.getWayPointCount(); ++waypoint_idx)
    {
      auto waypoint = trajectory.getWayPoint(waypoint_idx);
      output_file << waypoint.getVariablePosition(joint_group_indices.at(joint)) << std::endl;
    }
    output_file.clear();
    output_file.close();
  }
}

void TrackJointSmoothing::addTrackJointOutpointToRobotTrajectory(
    const robot_trajectory::RobotTrajectory& reference_trajectory, const size_t num_dof,
    const std::vector<int>& joint_group_indices, const std::vector<trackjoint::JointTrajectory>& trackjoint_output,
    robot_trajectory::RobotTrajectory& new_trajectory)
{
  // We will overwrite this waypoint
  moveit::core::RobotState waypoint = reference_trajectory.getWayPoint(0);

  for (size_t waypoint_idx = 1; waypoint_idx < trackjoint_output.size(); ++waypoint_idx)
  {
    for (size_t joint = 0; joint < num_dof; ++joint)
    {
      waypoint.setVariablePosition(joint_group_indices.at(joint), trackjoint_output.at(joint).positions(waypoint_idx));
      waypoint.setVariableVelocity(joint_group_indices.at(joint), trackjoint_output.at(joint).velocities(waypoint_idx));
      waypoint.setVariableAcceleration(joint_group_indices.at(joint),
                                       trackjoint_output.at(joint).accelerations(waypoint_idx));
    }
    new_trajectory.addSuffixWayPoint(waypoint, DEFAULT_TRACKJOINT_TIMESTEP);
  }
}

void TrackJointSmoothing::setTrackJointState(const size_t waypoint_idx,
                                             const robot_trajectory::RobotTrajectory& trajectory, const size_t num_dof,
                                             const std::vector<int>& joint_group_indices,
                                             std::vector<trackjoint::KinematicState>& joint_states)
{
  trackjoint::KinematicState joint_state;
  const moveit::core::RobotState waypoint = trajectory.getWayPoint(waypoint_idx);

  for (size_t joint = 0; joint < num_dof; ++joint)
  {
    joint_state.position = waypoint.getVariablePosition(joint_group_indices.at(joint));
    joint_state.velocity = waypoint.getVariableVelocity(joint_group_indices.at(joint));
    joint_state.acceleration = waypoint.getVariableAcceleration(joint_group_indices.at(joint));
    joint_states[joint] = joint_state;
  }
}

void TrackJointSmoothing::setTrackJointLimits(const moveit::core::JointModelGroup* group, const size_t num_dof,
                                              const double max_velocity_scaling_factor,
                                              const double max_acceleration_scaling_factor,
                                              std::vector<trackjoint::Limits>& limits)
{
  limits.resize(num_dof);

  const std::vector<std::string>& vars = group->getVariableNames();
  const moveit::core::RobotModel& rmodel = group->getParentModel();

  trackjoint::Limits single_joint_limits;
  for (size_t joint = 0; joint < num_dof; ++joint)
  {
    // TODO(andyz): read this from the joint group if/when jerk limits are added to the JointModel
    single_joint_limits.jerk_limit = DEFAULT_MAX_JERK;

    const moveit::core::VariableBounds& bounds = rmodel.getVariableBounds(vars.at(joint));

    // This assumes min/max bounds are symmetric
    if (bounds.velocity_bounded_)
    {
      single_joint_limits.velocity_limit = max_velocity_scaling_factor * bounds.max_velocity_;
    }
    else
    {
      single_joint_limits.velocity_limit = max_velocity_scaling_factor * DEFAULT_MAX_VELOCITY;
    }

    if (bounds.acceleration_bounded_)
    {
      single_joint_limits.acceleration_limit = max_acceleration_scaling_factor * bounds.max_acceleration_;
    }
    else
    {
      single_joint_limits.acceleration_limit = max_acceleration_scaling_factor * DEFAULT_MAX_ACCELERATION;
    }

    RCLCPP_WARN_STREAM(LOGGER, "jerk  limit: " << single_joint_limits.jerk_limit);
    RCLCPP_WARN_STREAM(LOGGER, "accel limit: " << single_joint_limits.acceleration_limit);
    RCLCPP_WARN_STREAM(LOGGER, "vel   limit: " << single_joint_limits.velocity_limit);

    limits.at(joint) = single_joint_limits;
  }
  RCLCPP_WARN_STREAM(LOGGER, "Done retrieving kinematic limits.");
}
}  // namespace trajectory_processing
