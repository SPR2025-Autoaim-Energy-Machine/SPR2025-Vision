// Copyright Chen Jun 2023. Licensed under the MIT License.
//
// Additional modifications and features by Chengfu Zou, Labor. Licensed under Apache License 2.0.
//
// Copyright (C) FYT Vision Group. All rights reserved.
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

#include "armor_solver/armor_solver_node.hpp"

#include <tf2/exceptions.h>
// std
#include <memory>
#include <rm_utils/heartbeat.hpp>
#include <vector>

namespace fyt::auto_aim {
ArmorSolverNode::ArmorSolverNode(const rclcpp::NodeOptions &options)
: Node("armor_solver", options), solver_(nullptr) {
  // Register logger
  FYT_REGISTER_LOGGER("armor_solver", "~/fyt2024-log", INFO);
  FYT_INFO("armor_solver", "Starting ArmorSolverNode!");

  debug_mode_ = this->declare_parameter("debug", true);

  // Tracker
  double max_match_distance = this->declare_parameter("tracker.max_match_distance", 0.2);
  double max_match_yaw_diff = this->declare_parameter("tracker.max_match_yaw_diff", 1.0);
  tracker_ = std::make_unique<Tracker>(max_match_distance, max_match_yaw_diff);
  tracker_->tracking_thres = this->declare_parameter("tracker.tracking_thres", 5);
  lost_time_thres_ = this->declare_parameter("tracker.lost_time_thres", 0.3);

  // EKF
  // xa = x_armor, xc = x_robot_center
  // state: xc, v_xc, yc, v_yc, za, v_za, yaw, v_yaw, r
  // measurement: xa, ya, za, yaw
  // f - Process function
  auto f = [this](const Eigen::VectorXd &x) {
    Eigen::VectorXd x_new = x;
    x_new(0) += x(1) * dt_;
    x_new(2) += x(3) * dt_;
    x_new(4) += x(5) * dt_;
    x_new(6) += x(7) * dt_;
    return x_new;
  };
  // J_f - Jacobian of process function
  auto j_f = [this](const Eigen::VectorXd &) {
    Eigen::MatrixXd f(9, 9);
    // clang-format off
    f <<  1,   dt_, 0,   0,   0,   0,   0,   0,   0,
          0,   1,   0,   0,   0,   0,   0,   0,   0,
          0,   0,   1,   dt_, 0,   0,   0,   0,   0, 
          0,   0,   0,   1,   0,   0,   0,   0,   0,
          0,   0,   0,   0,   1,   dt_, 0,   0,   0,
          0,   0,   0,   0,   0,   1,   0,   0,   0,
          0,   0,   0,   0,   0,   0,   1,   dt_, 0,
          0,   0,   0,   0,   0,   0,   0,   1,   0,
          0,   0,   0,   0,   0,   0,   0,   0,   1;
    // clang-format on
    return f;
  };
  // h - Observation function
  auto h = [](const Eigen::VectorXd &x) {
    Eigen::VectorXd z(4);
    double xc = x(0), yc = x(2), yaw = x(6), r = x(8);
    z(0) = xc - r * cos(yaw);  // xa
    z(1) = yc - r * sin(yaw);  // ya
    z(2) = x(4);               // za
    z(3) = x(6);               // yaw
    return z;
  };
  // J_h - Jacobian of observation function
  auto j_h = [](const Eigen::VectorXd &x) {
    Eigen::MatrixXd h(4, 9);
    double yaw = x(6), r = x(8);
    // clang-format off
    //    xc   v_xc yc   v_yc za   v_za yaw            v_yaw  r
    h <<  1,   0,   0,   0,   0,   0,   r*sin(yaw),  0,   -cos(yaw),
          0,   0,   1,   0,   0,   0,   -r*cos(yaw), 0,   -sin(yaw),
          0,   0,   0,   0,   1,   0,   0,              0,   0,
          0,   0,   0,   0,   0,   0,   1,              0,   0;
    // clang-format on
    return h;
  };
  // update_Q - process noise covariance matrix
  s2qx_ = declare_parameter("ekf.sigma2_q_x", 20.0);
  s2qy_ = declare_parameter("ekf.sigma2_q_y", 20.0);
  s2qz_ = declare_parameter("ekf.sigma2_q_z", 20.0);
  s2qyaw_ = declare_parameter("ekf.sigma2_q_yaw", 100.0);
  s2qr_ = declare_parameter("ekf.sigma2_q_r", 800.0);
  auto u_q = [this]() {
    Eigen::MatrixXd q(9, 9);
    double t = dt_, x = s2qx_, y = s2qy_, z = s2qz_, yaw = s2qyaw_, r = s2qr_;
    double q_x_x = pow(t, 4) / 4 * x, q_x_vx = pow(t, 3) / 2 * x, q_vx_vx = pow(t, 2) * x;
    double q_y_y = pow(t, 4) / 4 * y, q_y_vy = pow(t, 3) / 2 * y, q_vy_vy = pow(t, 2) * y;
    double q_z_z = pow(t, 4) / 4 * x, q_z_vz = pow(t, 3) / 2 * x, q_vz_vz = pow(t, 2) * z;
    double q_yaw_yaw = pow(t, 4) / 4 * yaw, q_yaw_vyaw = pow(t, 3) / 2 * x,
           q_vyaw_vyaw = pow(t, 2) * yaw;
    double q_r = pow(t, 4) / 4 * r;
    // clang-format off
    //    xc      v_xc    yc      v_yc    za      v_za    yaw         v_yaw       r
    q <<  q_x_x,  q_x_vx, 0,      0,      0,      0,      0,          0,          0,
          q_x_vx, q_vx_vx,0,      0,      0,      0,      0,          0,          0,
          0,      0,      q_y_y,  q_y_vy, 0,      0,      0,          0,          0,
          0,      0,      q_y_vy, q_vy_vy,0,      0,      0,          0,          0,
          0,      0,      0,      0,      q_z_z,  q_z_vz, 0,          0,          0,
          0,      0,      0,      0,      q_z_vz, q_vz_vz,0,          0,          0,
          0,      0,      0,      0,      0,      0,      q_yaw_yaw,  q_yaw_vyaw, 0,
          0,      0,      0,      0,      0,      0,      q_yaw_vyaw, q_vyaw_vyaw,0,
          0,      0,      0,      0,      0,      0,      0,          0,          q_r;
    // clang-format on
    return q;
  };
  // update_R - measurement noise covariance matrix
  r_x_ = declare_parameter("ekf.r_x", 0.05);
  r_y_ = declare_parameter("ekf.r_y", 0.05);
  r_z_ = declare_parameter("ekf.r_z", 0.05);
  r_yaw_ = declare_parameter("ekf.r_yaw", 0.02);
  auto u_r = [this](const Eigen::VectorXd &z) {
    Eigen::DiagonalMatrix<double, 4> r;
    r.diagonal() << abs(r_x_ * z[0]), abs(r_y_ * z[1]), abs(r_z_ * z[2]), r_yaw_;
    return r;
  };
  // P - error estimate covariance matrix
  Eigen::DiagonalMatrix<double, 9> p0;
  p0.setIdentity();
  tracker_->ekf = ExtendedKalmanFilter{f, h, j_f, j_h, u_q, u_r, p0};

  // Subscriber with tf2 message_filter
  // tf2 relevant
  tf2_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
  // Create the timer interface before call to waitForTransform,
  // to avoid a tf2_ros::CreateTimerInterfaceException exception
  auto timer_interface = std::make_shared<tf2_ros::CreateTimerROS>(
    this->get_node_base_interface(), this->get_node_timers_interface());
  tf2_buffer_->setCreateTimerInterface(timer_interface);
  tf2_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf2_buffer_);
  // subscriber and filter
  armors_sub_.subscribe(this, "armor_detector/armors", rmw_qos_profile_sensor_data);
  target_frame_ = this->declare_parameter("target_frame", "odom");
  tf2_filter_ = std::make_shared<tf2_filter>(armors_sub_,
                                             *tf2_buffer_,
                                             target_frame_,
                                             10,
                                             this->get_node_logging_interface(),
                                             this->get_node_clock_interface(),
                                             std::chrono::duration<int>(1));
  // Register a callback with tf2_ros::MessageFilter to be called when
  // transforms are available
  tf2_filter_->registerCallback(&ArmorSolverNode::armorsCallback, this);

  // Measurement publisher (for debug usage)
  measure_pub_ = this->create_publisher<rm_interfaces::msg::Measurement>("armor_solver/measurement",
                                                                         rclcpp::SensorDataQoS());

  // Publisher
  target_pub_ = this->create_publisher<rm_interfaces::msg::Target>("armor_solver/target",
                                                                   rclcpp::SensorDataQoS());
  gimbal_pub_ = this->create_publisher<rm_interfaces::msg::GimbalCmd>("armor_solver/cmd_gimbal",
                                                                      rclcpp::SensorDataQoS());

  // Visualization Marker Publisher
  // See http://wiki.ros.org/rviz/DisplayTypes/Marker
  position_marker_.ns = "position";
  position_marker_.type = visualization_msgs::msg::Marker::SPHERE;
  position_marker_.scale.x = position_marker_.scale.y = position_marker_.scale.z = 0.1;
  position_marker_.color.a = 1.0;
  position_marker_.color.g = 1.0;
  linear_v_marker_.type = visualization_msgs::msg::Marker::ARROW;
  linear_v_marker_.ns = "linear_v";
  linear_v_marker_.scale.x = 0.03;
  linear_v_marker_.scale.y = 0.05;
  linear_v_marker_.color.a = 1.0;
  linear_v_marker_.color.r = 1.0;
  linear_v_marker_.color.g = 1.0;
  angular_v_marker_.type = visualization_msgs::msg::Marker::ARROW;
  angular_v_marker_.ns = "angular_v";
  angular_v_marker_.scale.x = 0.03;
  angular_v_marker_.scale.y = 0.05;
  angular_v_marker_.color.a = 1.0;
  angular_v_marker_.color.b = 1.0;
  angular_v_marker_.color.g = 1.0;
  armors_marker_.ns = "filtered_armors";
  armors_marker_.type = visualization_msgs::msg::Marker::SPHERE_LIST;
  armors_marker_.scale.x = armors_marker_.scale.y = armors_marker_.scale.z = 0.1;
  armors_marker_.color.a = 1.0;
  armors_marker_.color.r = 1.0;
  aimming_line_marker_.ns = "aimming_line";
  aimming_line_marker_.type = visualization_msgs::msg::Marker::ARROW;
  aimming_line_marker_.scale.x = 0.03;
  aimming_line_marker_.scale.y = 0.05;
  aimming_line_marker_.color.a = 0.5;
  aimming_line_marker_.color.r = 1.0;
  aimming_line_marker_.color.b = 1.0;
  aimming_line_marker_.color.g = 1.0;
  trajectory_marker_.ns = "trajectory";
  trajectory_marker_.type = visualization_msgs::msg::Marker::POINTS;
  trajectory_marker_.scale.x = 0.01;
  trajectory_marker_.scale.y = 0.01;
  trajectory_marker_.color.a = 1.0;
  trajectory_marker_.color.r = 1.0;
  trajectory_marker_.color.g = 0.75;
  trajectory_marker_.color.b = 0.79;
  trajectory_marker_.points.clear();

  marker_pub_ =
    this->create_publisher<visualization_msgs::msg::MarkerArray>("armor_solver/marker", 10);

  // Heartbeat
  heartbeat_ = HeartBeatPublisher::create(this);
}

void ArmorSolverNode::armorsCallback(const rm_interfaces::msg::Armors::SharedPtr armors_msg) {
  // Lazy initialize solver owing to weak_from_this() can't be called in constructor
  if (solver_ == nullptr) {
    solver_ = std::make_unique<Solver>(weak_from_this());
  }

  // Tranform armor position from image frame to world coordinate
  for (auto &armor : armors_msg->armors) {
    geometry_msgs::msg::PoseStamped ps;
    ps.header = armors_msg->header;
    ps.pose = armor.pose;
    try {
      armor.pose = tf2_buffer_->transform(ps, target_frame_).pose;
    } catch (const tf2::TransformException &ex) {
      FYT_ERROR("armor_solver", "Transform error: {}", ex.what());
      return;
    }
  }

  // Filter abnormal armors
  armors_msg->armors.erase(std::remove_if(armors_msg->armors.begin(),
                                          armors_msg->armors.end(),
                                          [](const rm_interfaces::msg::Armor &armor) {
                                            return abs(armor.pose.position.z) > 2;
                                          }),
                           armors_msg->armors.end());

  // Init message
  rm_interfaces::msg::Measurement measure_msg;
  rm_interfaces::msg::Target target_msg;
  rclcpp::Time time = armors_msg->header.stamp;
  target_msg.header.stamp = time;
  target_msg.header.frame_id = target_frame_;

  // Update tracker
  if (tracker_->tracker_state == Tracker::LOST) {
    tracker_->init(armors_msg);
    target_msg.tracking = false;
  } else {
    dt_ = (time - last_time_).seconds();
    tracker_->lost_thres = std::abs(static_cast<int>(lost_time_thres_ / dt_));
    tracker_->update(armors_msg);
    // Publish measurement
    measure_msg.x = tracker_->measurement(0);
    measure_msg.y = tracker_->measurement(1);
    measure_msg.z = tracker_->measurement(2);
    measure_msg.yaw = tracker_->measurement(3);
    measure_pub_->publish(measure_msg);

    if (tracker_->tracker_state == Tracker::DETECTING) {
      target_msg.tracking = false;
    } else if (tracker_->tracker_state == Tracker::TRACKING ||
               tracker_->tracker_state == Tracker::TEMP_LOST) {
      target_msg.tracking = true;
      // Fill target message
      const auto &state = tracker_->target_state;
      target_msg.id = tracker_->tracked_id;
      target_msg.armors_num = static_cast<int>(tracker_->tracked_armors_num);
      target_msg.position.x = state(0);
      target_msg.velocity.x = state(1);
      target_msg.position.y = state(2);
      target_msg.velocity.y = state(3);
      target_msg.position.z = state(4);
      target_msg.velocity.z = state(5);
      target_msg.yaw = state(6);
      target_msg.v_yaw = state(7);
      target_msg.radius_1 = state(8);
      target_msg.radius_2 = tracker_->another_r;
      target_msg.dz = tracker_->dz;
    }
  }
  target_pub_->publish(target_msg);

  // Solve control command
  rm_interfaces::msg::GimbalCmd control_msg;
  if (target_msg.tracking) {
    try {
      control_msg = solver_->solve(target_msg, this->now(), tf2_buffer_);
    } catch (...) {
      FYT_ERROR("armor_solver", "Something went wrong in solver!");
      control_msg.yaw_diff = 0;
      control_msg.pitch_diff = 0;
      control_msg.distance = -1;
      control_msg.fire_advice = false;
    }
  } else {
    control_msg.yaw_diff = 0;
    control_msg.pitch_diff = 0;
    control_msg.distance = -1;
    control_msg.fire_advice = false;
  }
  gimbal_pub_->publish(control_msg);

  if (debug_mode_) {
    publishMarkers(target_msg, control_msg);
  }
  last_time_ = time;
}

void ArmorSolverNode::publishMarkers(const rm_interfaces::msg::Target &target_msg,
                                     const rm_interfaces::msg::GimbalCmd &gimbal_cmd) noexcept {
  position_marker_.header = target_msg.header;
  linear_v_marker_.header = target_msg.header;
  angular_v_marker_.header = target_msg.header;
  armors_marker_.header = target_msg.header;
  aimming_line_marker_.header = target_msg.header;
  aimming_line_marker_.header.frame_id = target_frame_ + "_rectify";

  if (target_msg.tracking) {
    double yaw = target_msg.yaw, r1 = target_msg.radius_1, r2 = target_msg.radius_2;
    double xc = target_msg.position.x, yc = target_msg.position.y, za = target_msg.position.z;
    double vx = target_msg.velocity.x, vy = target_msg.velocity.y, vz = target_msg.velocity.z;
    double dz = target_msg.dz;
    position_marker_.action = visualization_msgs::msg::Marker::ADD;
    position_marker_.pose.position.x = xc;
    position_marker_.pose.position.y = yc;
    position_marker_.pose.position.z = za + dz / 2;

    linear_v_marker_.action = visualization_msgs::msg::Marker::ADD;
    linear_v_marker_.points.clear();
    linear_v_marker_.points.emplace_back(position_marker_.pose.position);
    geometry_msgs::msg::Point arrow_end = position_marker_.pose.position;
    arrow_end.x += vx;
    arrow_end.y += vy;
    arrow_end.z += vz;
    linear_v_marker_.points.emplace_back(arrow_end);

    angular_v_marker_.action = visualization_msgs::msg::Marker::ADD;
    angular_v_marker_.points.clear();
    angular_v_marker_.points.emplace_back(position_marker_.pose.position);
    arrow_end = position_marker_.pose.position;
    arrow_end.z += target_msg.v_yaw / M_PI;
    angular_v_marker_.points.emplace_back(arrow_end);

    armors_marker_.action = visualization_msgs::msg::Marker::ADD;
    armors_marker_.points.clear();
    // Draw armors
    bool is_current_pair = true;
    size_t a_n = target_msg.armors_num;
    geometry_msgs::msg::Point p_a;
    double r = 0;
    for (size_t i = 0; i < a_n; i++) {
      double tmp_yaw = yaw + i * (2 * M_PI / a_n);
      // Only 4 armors has 2 radius and height
      if (a_n == 4) {
        r = is_current_pair ? r1 : r2;
        p_a.z = za + (is_current_pair ? 0 : dz);
        is_current_pair = !is_current_pair;
      } else {
        r = r1;
        p_a.z = za;
      }
      p_a.x = xc - r * cos(tmp_yaw);
      p_a.y = yc - r * sin(tmp_yaw);
      armors_marker_.points.emplace_back(p_a);
    }
    aimming_line_marker_.action = visualization_msgs::msg::Marker::ADD;
    aimming_line_marker_.points.clear();
    geometry_msgs::msg::Point aimming_line_start, aimming_line_end;
    aimming_line_marker_.points.emplace_back(aimming_line_start);
    aimming_line_end.y = 15 * sin(gimbal_cmd.yaw * M_PI / 180);
    aimming_line_end.x = 15 * cos(gimbal_cmd.yaw * M_PI / 180);
    aimming_line_end.z = 15 * sin(gimbal_cmd.pitch * M_PI / 180);
    aimming_line_marker_.points.emplace_back(aimming_line_end);
    if (gimbal_cmd.fire_advice) {
      aimming_line_marker_.color.r = 0;
      aimming_line_marker_.color.g = 1;
      aimming_line_marker_.color.b = 0;
    } else {
      aimming_line_marker_.color.r = 1;
      aimming_line_marker_.color.g = 1;
      aimming_line_marker_.color.b = 1;
    }

    trajectory_marker_.action = visualization_msgs::msg::Marker::ADD;
    trajectory_marker_.header.frame_id = "gimbal_link";
    trajectory_marker_.header.stamp = this->now();
    trajectory_marker_.points.clear();
    for (const auto &point :
         solver_->getTrajectory(gimbal_cmd.distance, gimbal_cmd.pitch * M_PI / 180)) {
      geometry_msgs::msg::Point p;
      p.x = point.first;
      p.z = point.second;
      trajectory_marker_.points.emplace_back(p);
    }

  } else {
    position_marker_.action = visualization_msgs::msg::Marker::DELETE;
    linear_v_marker_.action = visualization_msgs::msg::Marker::DELETE;
    angular_v_marker_.action = visualization_msgs::msg::Marker::DELETE;
    armors_marker_.action = visualization_msgs::msg::Marker::DELETE;
    trajectory_marker_.action = visualization_msgs::msg::Marker::DELETE;
    aimming_line_marker_.action = visualization_msgs::msg::Marker::DELETE;
  }

  visualization_msgs::msg::MarkerArray marker_array;

  marker_array.markers.emplace_back(position_marker_);
  marker_array.markers.emplace_back(trajectory_marker_);
  marker_array.markers.emplace_back(linear_v_marker_);
  marker_array.markers.emplace_back(angular_v_marker_);
  marker_array.markers.emplace_back(armors_marker_);
  marker_array.markers.emplace_back(aimming_line_marker_);
  marker_pub_->publish(marker_array);
}

}  // namespace fyt::auto_aim

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
// This acts as a sort of entry point, allowing the component to be discoverable
// when its library is being loaded into a running process.
RCLCPP_COMPONENTS_REGISTER_NODE(fyt::auto_aim::ArmorSolverNode)