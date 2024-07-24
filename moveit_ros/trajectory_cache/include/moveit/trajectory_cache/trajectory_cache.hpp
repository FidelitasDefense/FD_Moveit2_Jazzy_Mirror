// Copyright 2022 Johnson & Johnson
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include <rclcpp/rclcpp.hpp>

#include <warehouse_ros/message_collection.h>
#include <warehouse_ros/database_connection.h>

// TF2
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>

// ROS2 Messages
#include <geometry_msgs/msg/pose.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>

// moveit modules
#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit_msgs/srv/get_cartesian_path.hpp>

namespace moveit_ros
{
namespace trajectory_cache
{

/** \class TrajectoryCache trajectory_cache.hpp moveit/trajectory_cache/trajectory_cache.hpp
 *
 * \brief Trajectory Cache manager for MoveIt.
 *
 * This manager facilitates cache management for MoveIt 2's `MoveGroupInterface`
 * by using `warehouse_ros` to manage a database of executed trajectories, keyed
 * by the start and goal conditions, and sorted by how long the trajectories
 * took to execute. This allows for the lookup and reuse of the best performing
 * trajectories found so far.
 *
 * WARNING: RFE:
 *   !!! This cache does NOT support collision detection!
 *   Trajectories will be put into and fetched from the cache IGNORING
 *   collision.
 *
 *   If your planning scene is expected to change between cache lookups, do NOT
 *   use this cache, fetched trajectories are likely to result in collision
 *   then.
 *
 *   To handle collisions this class will need to hash the planning scene world
 *   msg (after zeroing out std_msgs/Header timestamps and sequences) and do an
 *   appropriate lookup, or do more complicated checks to see if the scene world
 *   is "close enough" or is a less obstructed version of the scene in the cache
 *   entry.
 *
 *   !!! This cache does NOT support keying on joint velocities and efforts.
 *   The cache only keys on joint positions.
 *
 *   !!! This cache does NOT support multi-DOF joints.

 *   !!! This cache does NOT support certain constraints
 *   Including: path, constraint regions, everything related to collision.
 *
 *   This is because they are difficult (but not impossible) to implement key
 *   logic for.
 *
 * Relevant ROS Parameters:
 *   - `warehouse_plugin`: What database to use
 *
 * This class supports trajectories planned from move_group MotionPlanRequests
 * as well as GetCartesianPath requests. That is, both normal motion plans and
 * cartesian plans are supported.
 *
 * Motion plan trajectories are stored in the `move_group_trajectory_cache`
 * database within the database file, with trajectories for each move group
 * stored in a collection named after the relevant move group's name.
 *
 * For example, the "my_move_group" move group will have its cache stored in
 * `move_group_trajectory_cache@my_move_group`
 *
 * Motion Plan Trajectories are keyed on:
 *   - Plan Start: robot joint state
 *   - Plan Goal (either of):
 *     - Final pose (wrt. `planning_frame` (usually `base_link`))
 *     - Final robot joint states
 *   - Plan Constraints (but not collision)
 *
 * Trajectories may be looked up with some tolerance at call time.
 *
 * Similarly, the cartesian trajectories are stored in the
 * `move_group_cartesian_trajectory_cache` database within the database file,
 * with trajectories for each move group stored in a collection named after the
 * relevant move group's name.
 *
 * Cartesian Trajectories are keyed on:
 *   - Plan Start: robot joint state
 *   - Plan Goal:
 *     - Pose waypoints
 */
class TrajectoryCache
{
public:
  /**
   * \brief Construct a TrajectoryCache.
   *
   * \param[in] node. An rclcpp::Node::SharedPtr, which will be used to lookup warehouse_ros parameters, log, and listen
   * for TF.
   *
   * TODO: methylDragon -
   *   We explicitly need a Node::SharedPtr because warehouse_ros ONLY supports it...
   *   Use rclcpp::node_interfaces::NodeInterfaces<> once warehouse_ros does.
   */
  explicit TrajectoryCache(const rclcpp::Node::SharedPtr& node);

  /**
   * \brief Initialize the TrajectoryCache.
   *
   * This sets up the database connection, and sets any configuration parameters.
   * You must call this before calling any other method of the trajectory cache.
   *
   * \param[in] db_path. The database path.
   * \param[in] db_port. The database port.
   * \param[in] exact_match_precision. Tolerance for float precision comparison for what counts as an exact match.
   *   An exact match is when:
   *   (candidate >= value - (exact_match_precision / 2) && candidate <= value + (exact_match_precision / 2))
   * \returns true if the database was successfully connected to.
   * */
  bool init(const std::string& db_path = ":memory:", uint32_t db_port = 0, double exact_match_precision = 1e-6);

  /**
   * \brief Count the number of non-cartesian trajectories for a particular cache namespace.
   *
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \returns The number of non-cartesian trajectories for the cache namespace.
   */
  unsigned countTrajectories(const std::string& cache_namespace);

  /**
   * \brief Count the number of cartesian trajectories for a particular cache namespace.
   *
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \returns The number of cartesian trajectories for the cache namespace.
   */
  unsigned countCartesianTrajectories(const std::string& cache_namespace);

  /**
   * \name Motion plan trajectory caching
   */
  /**@{*/

  /**
   * \brief Fetch all plans that fit within the requested tolerances for start and goal conditions, returning them as a
   * vector, sorted by some cache column.
   *
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \param[in] start_tolerance. Match tolerance for cache entries for the `plan_request` start parameters.
   * \param[in] goal_tolerance. Match tolerance for cache entries for the `plan_request` goal parameters.
   * \param[in] metadata_only. If true, returns only the cache entry metadata.
   * \param[in] sort_by. The cache column to sort by, defaults to execution time.
   * \param[in] ascending. If true, sorts in ascending order. If false, sorts in descending order.
   * \returns A vector of cache hits, sorted by the `sort_by` param.
   */
  std::vector<warehouse_ros::MessageWithMetadata<moveit_msgs::msg::RobotTrajectory>::ConstPtr>
  fetchAllMatchingTrajectories(const moveit::planning_interface::MoveGroupInterface& move_group,
                               const std::string& cache_namespace,
                               const moveit_msgs::msg::MotionPlanRequest& plan_request, double start_tolerance,
                               double goal_tolerance, bool metadata_only = false,
                               const std::string& sort_by = "execution_time_s", bool ascending = true);

  /**
   * \brief Fetch the best trajectory that fits within the requested tolerances for start and goal conditions, by some
   * cache column.
   *
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \param[in] start_tolerance. Match tolerance for cache entries for the `plan_request` start parameters.
   * \param[in] goal_tolerance. Match tolerance for cache entries for the `plan_request` goal parameters.
   * \param[in] metadata_only. If true, returns only the cache entry metadata.
   * \param[in] sort_by. The cache column to sort by, defaults to execution time.
   * \param[in] ascending. If true, sorts in ascending order. If false, sorts in descending order.
   * \returns The best cache hit, with respect to the `sort_by` param.
   */
  warehouse_ros::MessageWithMetadata<moveit_msgs::msg::RobotTrajectory>::ConstPtr fetchBestMatchingTrajectory(
      const moveit::planning_interface::MoveGroupInterface& move_group, const std::string& cache_namespace,
      const moveit_msgs::msg::MotionPlanRequest& plan_request, double start_tolerance, double goal_tolerance,
      bool metadata_only = false, const std::string& sort_by = "execution_time_s", bool ascending = true);

  /**
   * \brief Put a trajectory into the database if it is the best matching trajectory seen so far.
   *
   * Trajectories are matched based off their start and goal states.
   * And are considered "better" if they higher priority in the sorting order specified by `sort_by` than exactly matching trajectories.
   *
   * A trajectory is "exactly matching" if its start and goal are close enough to another trajectory.
   * The tolerance for this depends on the `exact_match_tolerance` arg passed in init().
   * \see init()
   *
   * Optionally deletes all worse trajectories by default to prune the cache.
   *
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \param[in] trajectory. The trajectory to put.
   * \param[in] execution_time_s. The execution time of the trajectory, in seconds.
   * \param[in] planning_time_s. How long the trajectory took to plan, in seconds.
   * \param[in] delete_worse_trajectories. If true, will prune the cache by deleting all cache entries that match the
   * `plan_request` exactly, if they are worse than the `trajectory`, even if it was not put.
   * \returns true if the trajectory was the best seen yet and hence put into the cache.
   */
  bool putTrajectory(const moveit::planning_interface::MoveGroupInterface& move_group,
                     const std::string& cache_namespace, const moveit_msgs::msg::MotionPlanRequest& plan_request,
                     const moveit_msgs::msg::RobotTrajectory& trajectory, double execution_time_s,
                     double planning_time_s, bool delete_worse_trajectories = true);

  /**@}*/

  /**
   * \name Cartesian trajectory caching
   */
  /**@{*/

  /**
   * \brief Construct a GetCartesianPath request.
   *
   * This mimics the move group computeCartesianPath signature (without path constraints).
   *
   * \param[in] move_group. The manipulator move group, used to get its state, frames, and link.
   * \param[in] waypoints. The cartesian waypoints to request the path for.
   * \param[in] max_step. The value to populate into the `GetCartesianPath` request's max_step field.
   * \param[in] jump_threshold. The value to populate into the `GetCartesianPath` request's jump_threshold field.
   * \param[in] avoid_collisions. The value to populate into the `GetCartesianPath` request's avoid_collisions field.
   * \returns
   */
  moveit_msgs::srv::GetCartesianPath::Request
  constructGetCartesianPathRequest(moveit::planning_interface::MoveGroupInterface& move_group,
                                   const std::vector<geometry_msgs::msg::Pose>& waypoints, double max_step,
                                   double jump_threshold, bool avoid_collisions = true);

  /**
   * \brief Fetch all cartesian trajectories that fit within the requested tolerances for start and goal conditions,
   * returning them as a vector, sorted by some cache column.
   *
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \param[in] min_fraction. The minimum fraction required for a cache hit.
   * \param[in] start_tolerance. Match tolerance for cache entries for the `plan_request` start parameters.
   * \param[in] goal_tolerance. Match tolerance for cache entries for the `plan_request` goal parameters.
   * \param[in] metadata_only. If true, returns only the cache entry metadata.
   * \param[in] sort_by. The cache column to sort by, defaults to execution time.
   * \param[in] ascending. If true, sorts in ascending order. If false, sorts in descending order.
   * \returns A vector of cache hits, sorted by the `sort_by` param.
   */
  std::vector<warehouse_ros::MessageWithMetadata<moveit_msgs::msg::RobotTrajectory>::ConstPtr>
  fetchAllMatchingCartesianTrajectories(const moveit::planning_interface::MoveGroupInterface& move_group,
                                        const std::string& cache_namespace,
                                        const moveit_msgs::srv::GetCartesianPath::Request& plan_request,
                                        double min_fraction, double start_tolerance, double goal_tolerance,
                                        bool metadata_only = false, const std::string& sort_by = "execution_time_s", bool ascending = true);

  /**
   * \brief Fetch the best cartesian trajectory that fits within the requested tolerances for start and goal conditions,
   * by some cache column.
   *
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \param[in] min_fraction. The minimum fraction required for a cache hit.
   * \param[in] start_tolerance. Match tolerance for cache entries for the `plan_request` start parameters.
   * \param[in] goal_tolerance. Match tolerance for cache entries for the `plan_request` goal parameters.
   * \param[in] metadata_only. If true, returns only the cache entry metadata.
   * \param[in] sort_by. The cache column to sort by, defaults to execution time.
   * \param[in] ascending. If true, sorts in ascending order. If false, sorts in descending order.
   * \returns The best cache hit, with respect to the `sort_by` param.
   */
  warehouse_ros::MessageWithMetadata<moveit_msgs::msg::RobotTrajectory>::ConstPtr fetchBestMatchingCartesianTrajectory(
      const moveit::planning_interface::MoveGroupInterface& move_group, const std::string& cache_namespace,
      const moveit_msgs::srv::GetCartesianPath::Request& plan_request, double min_fraction, double start_tolerance,
      double goal_tolerance, bool metadata_only = false, const std::string& sort_by = "execution_time_s", bool ascending = true);

  /**
   * \brief Put a cartesian trajectory into the database if it is the best matching cartesian trajectory seen so far.
   *
   * Cartesian trajectories are matched based off their start and goal states.
   * And are considered "better" if they higher priority in the sorting order specified by `sort_by` than exactly matching cartesian
   * trajectories.
   *
   * A trajectory is "exactly matching" if its start and goal (and fraction) are close enough to another trajectory.
   * The tolerance for this depends on the `exact_match_tolerance` arg passed in init().
   * \see init()
   *
   * Optionally deletes all worse cartesian trajectories by default to prune the cache.
   *
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] cache_namespace. A namespace to separate cache entries by. The name of the robot is a good choice.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \param[in] trajectory. The trajectory to put.
   * \param[in] execution_time_s. The execution time of the trajectory, in seconds.
   * \param[in] planning_time_s. How long the trajectory took to plan, in seconds.
   * \param[in] fraction. The fraction of the path that was computed.
   * \param[in] delete_worse_trajectories. If true, will prune the cache by deleting all cache entries that match the
   * `plan_request` exactly, if they are worse than `trajectory`, even if it was not put.
   * \returns true if the trajectory was the best seen yet and hence put into the cache.
   */
  bool putCartesianTrajectory(const moveit::planning_interface::MoveGroupInterface& move_group,
                              const std::string& cache_namespace,
                              const moveit_msgs::srv::GetCartesianPath::Request& plan_request,
                              const moveit_msgs::msg::RobotTrajectory& trajectory, double execution_time_s,
                              double planning_time_s, double fraction, bool delete_worse_trajectories = true);

  /**@}*/

private:
  /**
   * \name Motion plan trajectory query and metadata construction
   */
  /**@{*/

  /**
   * \brief Extract relevant parameters from a motion plan request's start parameters to populate a cache db query, with
   * some match tolerance.
   *
   * These parameters will be used to look-up relevant sections of a cache element's key.
   *
   * \param[out] query. The query to add parameters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \param[in] match_tolerance. The match tolerance (additive with exact_match_tolerance) for the query.
   * \returns true if successfully added to. If false, the query might have been partially modified and should not be
   * used.
   */
  bool extractAndAppendTrajectoryStartToQuery(warehouse_ros::Query& query,
                                              const moveit::planning_interface::MoveGroupInterface& move_group,
                                              const moveit_msgs::msg::MotionPlanRequest& plan_request,
                                              double match_tolerance);

  /**
   * \brief Extract relevant parameters from a motion plan request's goal parameters to populate a cache db query, with
   * some match tolerance.
   *
   * These parameters will be used to look-up relevant sections of a cache element's key.
   *
   * \param[out] query. The query to add parameters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \param[in] match_tolerance. The match tolerance (additive with exact_match_tolerance) for the query.
   * \returns true if successfully added to. If false, the query might have been partially modified and should not be
   * used.
   */
  bool extractAndAppendTrajectoryGoalToQuery(warehouse_ros::Query& query,
                                             const moveit::planning_interface::MoveGroupInterface& move_group,
                                             const moveit_msgs::msg::MotionPlanRequest& plan_request,
                                             double match_tolerance);

  /**
   * \brief Extract relevant parameters from a motion plan request's start parameters to populate a cache entry's
   * metadata.
   *
   * These parameters will be used key the cache element.
   *
   * \param[out] metadata. The metadata to add paramters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \returns true if successfully added to. If false, the metadata might have been partially modified and should not be
   * used.
   */
  bool extractAndAppendTrajectoryStartToMetadata(warehouse_ros::Metadata& metadata,
                                                 const moveit::planning_interface::MoveGroupInterface& move_group,
                                                 const moveit_msgs::msg::MotionPlanRequest& plan_request);

  /**
   * \brief Extract relevant parameters from a motion plan request's goal parameters to populate a cache entry's
   * metadata.
   *
   * These parameters will be used key the cache element.
   *
   * \param[out] metadata. The metadata to add paramters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The motion plan request to key the cache with.
   * \returns true if successfully added to. If false, the metadata might have been partially modified and should not be
   * used.
   */
  bool extractAndAppendTrajectoryGoalToMetadata(warehouse_ros::Metadata& metadata,
                                                const moveit::planning_interface::MoveGroupInterface& move_group,
                                                const moveit_msgs::msg::MotionPlanRequest& plan_request);

  /**@}*/

  /**
   * \name Cartesian trajectory query and metadata construction
   */
  /**@{*/

  /**
   * \brief Extract relevant parameters from a cartesian plan request's start parameters to populate a cache db query,
   * with some match tolerance.
   *
   * These parameters will be used to look-up relevant sections of a cache element's key.
   *
   * \param[out] query. The query to add parameters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \param[in] match_tolerance. The match tolerance (additive with exact_match_tolerance) for the query.
   * \returns true if successfully added to. If false, the query might have been partially modified and should not be
   * used.
   */
  bool extractAndAppendCartesianTrajectoryStartToQuery(warehouse_ros::Query& query,
                                                       const moveit::planning_interface::MoveGroupInterface& move_group,
                                                       const moveit_msgs::srv::GetCartesianPath::Request& plan_request,
                                                       double match_tolerance);

  /**
   * \brief Extract relevant parameters from a cartesian plan request's goal parameters to populate a cache db query,
   * with some match tolerance.
   *
   * These parameters will be used to look-up relevant sections of a cache element's key.
   *
   * \param[out] query. The query to add parameters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \param[in] match_tolerance. The match tolerance (additive with exact_match_tolerance) for the query.
   * \returns true if successfully added to. If false, the query might have been partially modified and should not be
   * used.
   */
  bool extractAndAppendCartesianTrajectoryGoalToQuery(warehouse_ros::Query& query,
                                                      const moveit::planning_interface::MoveGroupInterface& move_group,
                                                      const moveit_msgs::srv::GetCartesianPath::Request& plan_request,
                                                      double match_tolerance);

  /**
   * \brief Extract relevant parameters from a cartesian plan request's goal parameters to populate a cache entry's
   * metadata.
   *
   * These parameters will be used key the cache element.
   *
   * \param[out] metadata. The metadata to add paramters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \returns true if successfully added to. If false, the metadata might have been partially modified and should not be
   * used.
   */
  bool
  extractAndAppendCartesianTrajectoryStartToMetadata(warehouse_ros::Metadata& metadata,
                                                     const moveit::planning_interface::MoveGroupInterface& move_group,
                                                     const moveit_msgs::srv::GetCartesianPath::Request& plan_request);

  /**
   * \brief Extract relevant parameters from a cartesian plan request's goal parameters to populate a cache entry's
   * metadata.
   *
   * These parameters will be used key the cache element.
   *
   * \param[out] metadata. The metadata to add paramters to.
   * \param[in] move_group. The manipulator move group, used to get its state.
   * \param[in] plan_request. The cartesian plan request to key the cache with.
   * \returns true if successfully added to. If false, the metadata might have been partially modified and should not be
   * used.
   */
  bool
  extractAndAppendCartesianTrajectoryGoalToMetadata(warehouse_ros::Metadata& metadata,
                                                    const moveit::planning_interface::MoveGroupInterface& move_group,
                                                    const moveit_msgs::srv::GetCartesianPath::Request& plan_request);

  /**@}*/

  rclcpp::Node::SharedPtr node_;
  rclcpp::Logger logger_;
  warehouse_ros::DatabaseConnection::Ptr db_;

  double exact_match_precision_ = 1e-6;

  std::unique_ptr<tf2_ros::Buffer> tf_buffer_;
  std::shared_ptr<tf2_ros::TransformListener> tf_listener_;
};

}  // namespace trajectory_cache
}  // namespace moveit_ros
