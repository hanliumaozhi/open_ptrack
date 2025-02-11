/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2011-2012, Matteo Munaro [matteo.munaro@dei.unipd.it], Filippo Basso [filippo.basso@dei.unipd.it]
 * Copyright (c) 2013-, Open Perception, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 * * Neither the name of the copyright holder(s) nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Matteo Munaro [matteo.munaro@dei.unipd.it], Filippo Basso [filippo.basso@dei.unipd.it]
 *
 */

#include <ros/ros.h>
#include <opencv2/opencv.hpp>
#include <Eigen/Eigen>
#include <visualization_msgs/MarkerArray.h>
#include <std_msgs/Bool.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <list>
#include <sstream>
#include <fstream>
#include <string.h>

#include <open_ptrack/opt_utils/conversions.h>
#include <open_ptrack/detection/detection.h>
#include <open_ptrack/detection/detection_source.h>
#include <open_ptrack/tracking/tracker.h>
#include <opt_msgs/Detection.h>
#include <opt_msgs/DetectionArray.h>
#include <opt_msgs/TrackArray.h>
//#include <open_ptrack/opt_utils/ImageConverter.h>

// Global variables:
std::map<std::string, open_ptrack::detection::DetectionSource*> detection_sources_map;
tf::TransformListener* tf_listener;
std::string world_frame_id;
bool output_history_pointcloud;
int output_history_size;
bool output_markers;
bool output_image_rgb;
bool output_tracking_results;
bool vertical;
ros::Publisher results_pub;
ros::Publisher marker_pub_tmp;
ros::Publisher marker_pub;
ros::Publisher pointcloud_pub;
size_t starting_index;
tf::Transform camera_frame_to_world_transform;
tf::Transform world_to_camera_frame_transform;
bool extrinsic_calibration;
double period;
open_ptrack::tracking::Tracker* tracker;
pcl::PointCloud<pcl::PointXYZRGB>::Ptr history_pointcloud(new pcl::PointCloud<pcl::PointXYZRGB>);
bool swissranger;
double min_confidence;
double min_confidence_sr;
double min_confidence_detections;
double min_confidence_detections_sr;

/**
 * \brief Read the DetectionArray message and use the detections for creating/updating/deleting tracks
 *
 * \param[in] msg the DetectionArray message.
 */
void
detection_cb(const opt_msgs::DetectionArray::ConstPtr& msg)
{
  // Read message header information:
  std::string frame_id = msg->header.frame_id;
  ros::Time frame_time = msg->header.stamp;

  tf::StampedTransform transform;
  tf::StampedTransform inverse_transform;
  //	cv_bridge::CvImage::Ptr cvPtr;

  try
  {
    // Read transforms between camera frame and world frame:
    if (!extrinsic_calibration)
    {
      static tf::TransformBroadcaster world_to_camera_tf_publisher;
//      world_to_camera_tf_publisher.sendTransform(tf::StampedTransform(camera_frame_to_world_transform, ros::Time::now(), world_frame_id, frame_id));
      world_to_camera_tf_publisher.sendTransform(tf::StampedTransform(world_to_camera_frame_transform, ros::Time::now(), frame_id, world_frame_id));
    }

    //Calculate direct and inverse transforms between camera and world frame:
    tf_listener->lookupTransform(world_frame_id, frame_id, ros::Time(0), transform);
    tf_listener->lookupTransform(frame_id, world_frame_id, ros::Time(0), inverse_transform);

    //		cvPtr = cv_bridge::toCvCopy(msg->image, sensor_msgs::image_encodings::BGR8);

    // Read camera intrinsic parameters:
    Eigen::Matrix3d intrinsic_matrix;
    for(int i = 0; i < 3; i++)
      for(int j = 0; j < 3; j++)
        intrinsic_matrix(i, j) = msg->intrinsic_matrix[i * 3 + j];

    // Add a new DetectionSource or update an existing one:
    if(detection_sources_map.find(frame_id) == detection_sources_map.end())
    {
      detection_sources_map[frame_id] = new open_ptrack::detection::DetectionSource(cv::Mat(0, 0, CV_8UC3),
          transform, inverse_transform, intrinsic_matrix, frame_time, frame_id);
    }
    else
    {
      detection_sources_map[frame_id]->update(cv::Mat(0, 0, CV_8UC3), transform, inverse_transform,
          intrinsic_matrix, frame_time, frame_id);
      double d = detection_sources_map[frame_id]->getDuration().toSec() / period;
      int lostFrames = int(round(d)) - 1;
    }
    open_ptrack::detection::DetectionSource* source = detection_sources_map[frame_id];

    // Create a Detection object for every detection in the detection message:
    std::vector<open_ptrack::detection::Detection> detections_vector;
    for(std::vector<opt_msgs::Detection>::const_iterator it = msg->detections.begin();
        it != msg->detections.end(); it++)
    {
      detections_vector.push_back(open_ptrack::detection::Detection(*it, source));
    }

    // If tracking in a network containing SwissRangers, convert SwissRanger people detection confidence from HOG-like to HaarDispAda-like:
    if (!std::strcmp(msg->header.frame_id.substr(0, 2).c_str(), "SR"))
    {
      for(unsigned int i = 0; i < detections_vector.size(); i++)
      {
        double new_confidence = detections_vector[i].getConfidence();
        new_confidence = (new_confidence - min_confidence_detections_sr) / (min_confidence_sr - min_confidence_detections_sr) *
                         (min_confidence - min_confidence_detections) + min_confidence_detections;
        detections_vector[i].setConfidence(new_confidence);
      }
    }

    // If at least one detection has been received:
    if(detections_vector.size() > 0)
    {
      // Perform detection-track association:
      tracker->newFrame(detections_vector);
      tracker->updateTracks();

      // Create a TrackingResult message with the output of the tracking process
      if(output_tracking_results)
      {
        opt_msgs::TrackArray::Ptr tracking_results_msg(new opt_msgs::TrackArray);
        tracking_results_msg->header.stamp = frame_time;
        tracking_results_msg->header.frame_id = world_frame_id;
        tracker->toMsg(tracking_results_msg);
        // Publish tracking message:
        results_pub.publish(tracking_results_msg);
      }

//      //Show the tracking process' results as an image
//      if(output_image_rgb)
//      {
//        tracker->drawRgb();
//        for(std::map<std::string, open_ptrack::detection::DetectionSource*>::iterator
//            it = detection_sources_map.begin(); it != detection_sources_map.end(); it++)
//        {
//          cv::Mat image_to_show = it->second->getImage();
//          if (not vertical)
//          {
//            //cv::imshow("TRACKER " + it->first, image_to_show);
//            cv::imshow("TRACKER ", image_to_show);		// TODO: use the above row if using multiple cameras
//          }
//          else
//          {
//            cv::flip(image_to_show.t(), image_to_show, -1);
//            cv::flip(image_to_show, image_to_show, 1);
//            //cv::imshow("TRACKER " + it->first, image_to_show);
//            cv::imshow("TRACKER ", image_to_show);		// TODO: use the above row if using multiple cameras
//          }
//          cv::waitKey(2);
//        }
//      }

      // Show the pose of each tracked object with a 3D marker (to be visualized with ROS RViz)
      if(output_markers)
      {
        visualization_msgs::MarkerArray::Ptr marker_msg(new visualization_msgs::MarkerArray);
        tracker->toMarkerArray(marker_msg);
        marker_pub.publish(marker_msg);
      }

      // Show the history of the movements in 3D (3D trajectory) of each tracked object as a PointCloud (which can be visualized in RViz)
      if(output_history_pointcloud)
      {
        history_pointcloud->header.stamp = frame_time.toNSec() / 1e3;  // Convert from ns to us
        history_pointcloud->header.frame_id = world_frame_id;
        starting_index = tracker->appendToPointCloud(history_pointcloud, starting_index,
            output_history_size);
        pointcloud_pub.publish(history_pointcloud);
      }
    }
    else // if no detections have been received
    {
      if(output_tracking_results)
      { // Publish an empty tracking message
        opt_msgs::TrackArray::Ptr tracking_results_msg(new opt_msgs::TrackArray);
        tracking_results_msg->header.stamp = frame_time;
        tracking_results_msg->header.frame_id = world_frame_id;
        results_pub.publish(tracking_results_msg);
      }
    }
  }
//  catch(cv_bridge::Exception& ex)
//  {
//    ROS_ERROR("cv_bridge exception: %s", ex.what());
//  }
  catch(tf::TransformException& ex)
  {
    ROS_ERROR("transform exception: %s", ex.what());
  }
}

void
fillChiMap(std::map<double, double>& chi_map, bool velocity_in_motion_term)
{
  if (velocity_in_motion_term)		// chi square values with state dimension = 4
  {
    chi_map[0.5] = 3.357;
    chi_map[0.75] = 5.385;
    chi_map[0.8] = 5.989;
    chi_map[0.9] = 7.779;
    chi_map[0.95] = 9.488;
    chi_map[0.98] = 11.668;
    chi_map[0.99] = 13.277;
    chi_map[0.995] = 14.860;
    chi_map[0.998] = 16.924;
    chi_map[0.999] = 18.467;
  }
  else							              // chi square values with state dimension = 2
  {
    chi_map[0.5] = 1.386;
    chi_map[0.75] = 2.773;
    chi_map[0.8] = 3.219;
    chi_map[0.9] = 4.605;
    chi_map[0.95] = 5.991;
    chi_map[0.98] = 7.824;
    chi_map[0.99] = 9.210;
    chi_map[0.995] = 10.597;
    chi_map[0.998] = 12.429;
    chi_map[0.999] = 13.816;
  }
}

int
main(int argc, char** argv)
{
  //Chi square distribution
  std::map<double, double> chi_map;

  ros::init(argc, argv, "tracker");
  ros::NodeHandle nh("~");

  // Subscribers/Publishers:
  ros::Subscriber input_sub = nh.subscribe("input", 5, detection_cb);
  marker_pub_tmp = nh.advertise<visualization_msgs::Marker>("/tracker/markers", 1);
  marker_pub = nh.advertise<visualization_msgs::MarkerArray>("/tracker/markers_array", 1);
  pointcloud_pub = nh.advertise<pcl::PointCloud<pcl::PointXYZRGBA> >("/tracker/history", 1);
  results_pub = nh.advertise<opt_msgs::TrackArray>("/tracker/tracks", 1);

  tf_listener = new tf::TransformListener();

  // Read tracking parameters:
  nh.param("world_frame_id", world_frame_id, std::string("/odom"));

  nh.param("orientation/vertical", vertical, false);
  nh.param("extrinsic_calibration", extrinsic_calibration, false);

  double voxel_size;
  nh.param("voxel_size", voxel_size, 0.075);

  double rate;
  nh.param("rate", rate, 30.0);

  int num_cameras;
  nh.param("num_cameras", num_cameras, 1);

//  double min_confidence;
  nh.param("min_confidence_initialization", min_confidence, -2.5); //0.0);

  double chi_value;
  nh.param("kalman/gate_distance_probability", chi_value, 0.9);

  double acceleration_variance;
  nh.param("kalman/acceleration_variance", acceleration_variance, 1.0);

  bool detector_likelihood;
  nh.param("jointLikelihood/detector_likelihood", detector_likelihood, false);

  bool velocity_in_motion_term;
  nh.param("jointLikelihood/velocity_in_motion_term", velocity_in_motion_term, false);

  double detector_weight;
  nh.param("jointLikelihood/detector_weight", detector_weight, -1.0);

  double motion_weight;
  nh.param("jointLikelihood/motion_weight", motion_weight, 0.5);

  double sec_before_old;
  nh.param("target/sec_before_old", sec_before_old, 3.6);

  double sec_before_fake;
  nh.param("target/sec_before_fake", sec_before_fake, 2.4);

  double sec_remain_new;
  nh.param("target/sec_remain_new", sec_remain_new, 1.2);

  int detections_to_validate;
  nh.param("target/detections_to_validate", detections_to_validate, 5);

  double haar_disp_ada_min_confidence, ground_based_people_detection_min_confidence;
  nh.param("haar_disp_ada_min_confidence", haar_disp_ada_min_confidence, -2.5); //0.0);
  nh.param("ground_based_people_detection_min_confidence", ground_based_people_detection_min_confidence, -2.5); //0.0);

  nh.param("swissranger", swissranger, false);

  nh.param("ground_based_people_detection_min_confidence_sr", min_confidence_detections_sr, -1.5);
  nh.param("min_confidence_initialization_sr", min_confidence_sr, -1.1);

  nh.param("output/history_pointcloud", output_history_pointcloud, false);
  nh.param("output/history_size", output_history_size, 0);
  nh.param("output/markers", output_markers, true);
  nh.param("output/image_rgb", output_image_rgb, true);
  nh.param("output/tracking_results", output_tracking_results, true);

  bool debug_mode;
  nh.param("debug/active", debug_mode, false);

  // Set min_confidence_detections variable based on sensor type:
  if (swissranger)
    min_confidence_detections = ground_based_people_detection_min_confidence;
  else
    min_confidence_detections = haar_disp_ada_min_confidence;

  // Take chi square values with regards to the state dimension:
  fillChiMap(chi_map, velocity_in_motion_term);

  // Compute additional parameters:
  period = 1.0 / rate;
  double gate_distance;
  gate_distance = chi_map.find(chi_value) != chi_map.end() ? chi_map[chi_value] : chi_map[0.999];

  double position_variance;
//  position_variance = 3*std::pow(2 * voxel_size, 2) / 12.0; // DEFAULT
  position_variance = 30*std::pow(2 * voxel_size, 2) / 12.0;
  std::vector<double> likelihood_weights;
  likelihood_weights.push_back(detector_weight*chi_map[0.999]/18.467);
  likelihood_weights.push_back(motion_weight);

  ros::Rate hz(num_cameras*rate);

//  cv::namedWindow("TRACKER ", CV_WINDOW_NORMAL);

  // Initialize an instance of the Tracker object:
  tracker = new open_ptrack::tracking::Tracker(
      gate_distance,
      detector_likelihood,
      likelihood_weights,
      velocity_in_motion_term,
      min_confidence,
      min_confidence_detections,
      sec_before_old,
      sec_before_fake,
      sec_remain_new,
      detections_to_validate,
      period,
      position_variance,
      acceleration_variance,
      world_frame_id,
      debug_mode,
      vertical);

  starting_index = 0;

  // If extrinsic calibration is not available:
  if (!extrinsic_calibration)
  { // Set fixed transformation from rgb frame and base_link
    tf::Vector3 fixed_translation(0, 0, 0);                  // camera_rgb_optical_frame -> world
    tf::Quaternion fixed_rotation(-0.5, 0.5, -0.5, -0.5);	// camera_rgb_optical_frame -> world
    tf::Vector3 inv_fixed_translation(0.0, 0.0, 0);			// world -> camera_rgb_optical_frame
    tf::Quaternion inv_fixed_rotation(-0.5, 0.5, -0.5, 0.5);	// world -> camera_rgb_optical_frame
    world_to_camera_frame_transform = tf::Transform(fixed_rotation, fixed_translation);
    camera_frame_to_world_transform = tf::Transform(inv_fixed_rotation, inv_fixed_translation);
  }

  // Spin and execute callbacks:
  ros::spin();

  return 0;
}
