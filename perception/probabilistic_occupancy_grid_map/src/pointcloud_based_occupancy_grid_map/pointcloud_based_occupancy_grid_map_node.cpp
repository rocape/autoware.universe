// Copyright 2021 Tier IV, Inc.
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

#include "pointcloud_based_occupancy_grid_map/pointcloud_based_occupancy_grid_map_node.hpp"

#include "cost_value.hpp"
#include "utils/utils.hpp"

#include <pcl_ros/transforms.hpp>
#include <tier4_autoware_utils/tier4_autoware_utils.hpp>

#include <nav_msgs/msg/occupancy_grid.hpp>

#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#ifdef ROS_DISTRO_GALACTIC
#include <tf2_eigen/tf2_eigen.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#else
#include <tf2_eigen/tf2_eigen.hpp>

#include <tf2_sensor_msgs/tf2_sensor_msgs.hpp>
#endif

#include <algorithm>
#include <memory>
#include <string>

namespace occupancy_grid_map
{
using costmap_2d::OccupancyGridMap;
using costmap_2d::OccupancyGridMapBBFUpdater;
using geometry_msgs::msg::Pose;

PointcloudBasedOccupancyGridMapNode::PointcloudBasedOccupancyGridMapNode(
  const rclcpp::NodeOptions & node_options)
: Node("pointcloud_based_occupancy_grid_map_node", node_options)
{
  using std::placeholders::_1;
  using std::placeholders::_2;

  /* params */
  map_frame_ = declare_parameter("map_frame", "map");
  base_link_frame_ = declare_parameter("base_link_frame", "base_link");
  gridmap_origin_frame_ = declare_parameter("gridmap_origin_frame", "base_link");
  scan_origin_frame_ = declare_parameter("scan_origin_frame", "base_link");
  use_height_filter_ = declare_parameter("use_height_filter", true);
  enable_single_frame_mode_ = declare_parameter("enable_single_frame_mode", false);
  filter_obstacle_pointcloud_by_raw_pointcloud_ =
    declare_parameter("filter_obstacle_pointcloud_by_raw_pointcloud", false);
  const double map_length{declare_parameter("map_length", 100.0)};
  const double map_resolution{declare_parameter("map_resolution", 0.5)};

  /* Subscriber and publisher */
  obstacle_pointcloud_sub_.subscribe(
    this, "~/input/obstacle_pointcloud",
    rclcpp::SensorDataQoS{}.keep_last(1).get_rmw_qos_profile());
  raw_pointcloud_sub_.subscribe(
    this, "~/input/raw_pointcloud", rclcpp::SensorDataQoS{}.keep_last(1).get_rmw_qos_profile());
  sync_ptr_ = std::make_shared<Sync>(SyncPolicy(5), obstacle_pointcloud_sub_, raw_pointcloud_sub_);

  sync_ptr_->registerCallback(
    std::bind(&PointcloudBasedOccupancyGridMapNode::onPointcloudWithObstacleAndRaw, this, _1, _2));
  occupancy_grid_map_pub_ = create_publisher<OccupancyGrid>("~/output/occupancy_grid_map", 1);

  /* Occupancy grid */
  occupancy_grid_map_updater_ptr_ = std::make_shared<OccupancyGridMapBBFUpdater>(
    map_length / map_resolution, map_length / map_resolution, map_resolution);

  // initialize debug tool
  {
    using tier4_autoware_utils::DebugPublisher;
    using tier4_autoware_utils::StopWatch;
    stop_watch_ptr_ = std::make_unique<StopWatch<std::chrono::milliseconds>>();
    debug_publisher_ptr_ =
      std::make_unique<DebugPublisher>(this, "pointcloud_based_occupancy_grid_map");
    stop_watch_ptr_->tic("cyclic_time");
    stop_watch_ptr_->tic("processing_time");
  }
}

void PointcloudBasedOccupancyGridMapNode::onPointcloudWithObstacleAndRaw(
  const PointCloud2::ConstSharedPtr & input_obstacle_msg,
  const PointCloud2::ConstSharedPtr & input_raw_msg)
{
  if (stop_watch_ptr_) {
    stop_watch_ptr_->toc("processing_time", true);
  }
  // Apply height filter
  PointCloud2 cropped_obstacle_pc{};
  PointCloud2 cropped_raw_pc{};
  if (use_height_filter_) {
    constexpr float min_height = -1.0, max_height = 2.0;
    if (!utils::cropPointcloudByHeight(
          *input_obstacle_msg, *tf2_, base_link_frame_, min_height, max_height,
          cropped_obstacle_pc)) {
      return;
    }
    if (!utils::cropPointcloudByHeight(
          *input_raw_msg, *tf2_, base_link_frame_, min_height, max_height, cropped_raw_pc)) {
      return;
    }
  }
  const PointCloud2 & filtered_obstacle_pc =
    use_height_filter_ ? cropped_obstacle_pc : *input_obstacle_msg;
  const PointCloud2 & filtered_raw_pc = use_height_filter_ ? cropped_raw_pc : *input_raw_msg;

  // Filter obstacle pointcloud by raw pointcloud
  PointCloud2 filtered_obstacle_pc_common{};
  if (filter_obstacle_pointcloud_by_raw_pointcloud_) {
    if (!utils::extractCommonPointCloud(
          filtered_obstacle_pc, filtered_raw_pc, filtered_obstacle_pc_common)) {
      filtered_obstacle_pc_common = filtered_obstacle_pc;
    }
  } else {
    filtered_obstacle_pc_common = filtered_obstacle_pc;
  }

  // Get from map to sensor frame pose
  Pose robot_pose{};
  Pose gridmap_origin{};
  Pose scan_origin{};
  try {
    robot_pose = utils::getPose(input_raw_msg->header, *tf2_, map_frame_);
    gridmap_origin =
      utils::getPose(input_raw_msg->header.stamp, *tf2_, gridmap_origin_frame_, map_frame_);
    scan_origin =
      utils::getPose(input_raw_msg->header.stamp, *tf2_, scan_origin_frame_, map_frame_);
  } catch (tf2::TransformException & ex) {
    RCLCPP_WARN_STREAM(get_logger(), ex.what());
    return;
  }

  // Create single frame occupancy grid map
  OccupancyGridMap single_frame_occupancy_grid_map(
    occupancy_grid_map_updater_ptr_->getSizeInCellsX(),
    occupancy_grid_map_updater_ptr_->getSizeInCellsY(),
    occupancy_grid_map_updater_ptr_->getResolution());
  single_frame_occupancy_grid_map.updateOrigin(
    gridmap_origin.position.x - single_frame_occupancy_grid_map.getSizeInMetersX() / 2,
    gridmap_origin.position.y - single_frame_occupancy_grid_map.getSizeInMetersY() / 2);
  single_frame_occupancy_grid_map.updateWithPointCloud(
    filtered_raw_pc, filtered_obstacle_pc_common, robot_pose, scan_origin);

  if (enable_single_frame_mode_) {
    // publish
    occupancy_grid_map_pub_->publish(OccupancyGridMapToMsgPtr(
      map_frame_, input_raw_msg->header.stamp, robot_pose.position.z,
      single_frame_occupancy_grid_map));  // (todo) robot_pose may be altered with gridmap_origin
  } else {
    // Update with bayes filter
    occupancy_grid_map_updater_ptr_->update(single_frame_occupancy_grid_map);

    // publish
    occupancy_grid_map_pub_->publish(OccupancyGridMapToMsgPtr(
      map_frame_, input_raw_msg->header.stamp, robot_pose.position.z,
      *occupancy_grid_map_updater_ptr_));
  }

  if (debug_publisher_ptr_ && stop_watch_ptr_) {
    const double cyclic_time_ms = stop_watch_ptr_->toc("cyclic_time", true);
    const double processing_time_ms = stop_watch_ptr_->toc("processing_time", true);
    debug_publisher_ptr_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/cyclic_time_ms", cyclic_time_ms);
    debug_publisher_ptr_->publish<tier4_debug_msgs::msg::Float64Stamped>(
      "debug/processing_time_ms", processing_time_ms);
  }
}

OccupancyGrid::UniquePtr PointcloudBasedOccupancyGridMapNode::OccupancyGridMapToMsgPtr(
  const std::string & frame_id, const Time & stamp, const float & robot_pose_z,
  const Costmap2D & occupancy_grid_map)
{
  auto msg_ptr = std::make_unique<OccupancyGrid>();

  msg_ptr->header.frame_id = frame_id;
  msg_ptr->header.stamp = stamp;
  msg_ptr->info.resolution = occupancy_grid_map.getResolution();

  msg_ptr->info.width = occupancy_grid_map.getSizeInCellsX();
  msg_ptr->info.height = occupancy_grid_map.getSizeInCellsY();

  double wx{};
  double wy{};
  occupancy_grid_map.mapToWorld(0, 0, wx, wy);
  msg_ptr->info.origin.position.x = occupancy_grid_map.getOriginX();
  msg_ptr->info.origin.position.y = occupancy_grid_map.getOriginY();
  msg_ptr->info.origin.position.z = robot_pose_z;
  msg_ptr->info.origin.orientation.w = 1.0;

  msg_ptr->data.resize(msg_ptr->info.width * msg_ptr->info.height);

  unsigned char * data = occupancy_grid_map.getCharMap();
  for (unsigned int i = 0; i < msg_ptr->data.size(); ++i) {
    msg_ptr->data[i] = occupancy_cost_value::cost_translation_table[data[i]];
  }
  return msg_ptr;
}

}  // namespace occupancy_grid_map

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(occupancy_grid_map::PointcloudBasedOccupancyGridMapNode)
