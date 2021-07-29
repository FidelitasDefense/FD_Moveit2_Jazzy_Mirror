/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2019, Los Alamos National Security, LLC
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

/*      Title     : enforce_limits.cpp
 *      Project   : moveit_servo
 *      Created   : 7/5/2021
 *      Author    : Brian O'Neil, Andy Zelenak, Blake Anderson, Tyler Weaver
 */

#include <moveit/robot_model/joint_model_group.h>
#include <moveit_servo/enforce_limits.hpp>

#include <sensor_msgs/msg/joint_state.hpp>

#include <Eigen/Core>

namespace moveit_servo
{
namespace
{
double getVelocityScalingFactor(const moveit::core::JointModelGroup* joint_model_group, const Eigen::ArrayXd& velocity)
{
  std::size_t joint_delta_index{ 0 };
  double velocity_scaling_factor{ 1.0 };
  for (const moveit::core::JointModel* joint : joint_model_group->getActiveJointModels())
  {
    const auto& bounds = joint->getVariableBounds(joint->getName());
    if (bounds.velocity_bounded_ && velocity(joint_delta_index) != 0.0)
    {
      const double unbounded_velocity = velocity(joint_delta_index);
      // Clamp each joint velocity to a joint specific [min_velocity, max_velocity] range.
      const auto bounded_velocity = std::min(std::max(unbounded_velocity, bounds.min_velocity_), bounds.max_velocity_);
      velocity_scaling_factor = std::min(velocity_scaling_factor, bounded_velocity / unbounded_velocity);
    }
    ++joint_delta_index;
  }

  return velocity_scaling_factor;
}

}  // namespace

Eigen::ArrayXd enforceVelocityLimits(const moveit::core::JointModelGroup* joint_model_group, double publish_period,
                                     const Eigen::ArrayXd& delta_theta)
{
  // Convert to joint angle velocities for checking and applying joint specific velocity limits.
  Eigen::ArrayXd velocity = delta_theta / publish_period;

  // Get the velocity scaling factor
  double velocity_scaling_factor = getVelocityScalingFactor(joint_model_group, velocity);

  // Scale the resulting detas to avoid violating limits.
  return velocity_scaling_factor * velocity * publish_period;
}

}  // namespace moveit_servo
