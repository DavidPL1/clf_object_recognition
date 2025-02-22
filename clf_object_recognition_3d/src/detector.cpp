#include "clf_object_recognition_3d/detector.h"

#include <math.h>
#include <filesystem>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/obj_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/common/centroid.h>
#include <pcl/common/eigen.h>
#include <pcl/registration/gicp.h>
#include <pcl/filters/crop_box.h>
#include <pcl_conversions/pcl_conversions.h>

#include <ros/console.h>

#include <Eigen/Geometry>

#include <cv_bridge/cv_bridge.h>
#include <image_geometry/pinhole_camera_model.h>

#include <eigen_conversions/eigen_msg.h>
#include <tf2_eigen/tf2_eigen.h>

#include <geometry_msgs/Pose.h>
#include <geometry_msgs/TransformStamped.h>

#include "clf_object_recognition_msgs/Detect2DImage.h"

#include "clf_object_recognition_3d/load_cloud.hpp"
#include "clf_object_recognition_3d/cloud_from_image.h"

inline bool validateFloats(double val)
{
  return !(std::isnan(val) || std::isinf(val));
}

Detector::Detector(ros::NodeHandle nh)
  : sync_(image_sub_, depth_image_sub_, camera_info_sub_, 10), tf_listener(tf_buffer)
{
  auto f = [this](auto&& PH1, auto&& PH2) { ReconfigureCallback(PH1, PH2); };
  reconfigure_server.setCallback(f);
  // get configuration first
  ros::spinOnce();

  srv_detect_2d = nh.serviceClient<clf_object_recognition_msgs::Detect2DImage>("/yolox/recognize_from_image");
  srv_detect_3d = nh.advertiseService("simple_detections", &Detector::ServiceDetect3D, this);

  pub_detections_3d = nh.advertise<vision_msgs::Detection3DArray>("last_detection", 1);
  pub_marker = nh.advertise<visualization_msgs::MarkerArray>("objects", 1);
  pub_cloud = nh.advertise<sensor_msgs::PointCloud2>("cloud", 1);

  // vision_msgs::Detection3DArray

  // subscribe to camera topics
  image_sub_.subscribe(nh, config.image_topic, 1);
  depth_image_sub_.subscribe(nh, config.depth_topic, 1);
  camera_info_sub_.subscribe(nh, config.info_topic, 1);

  // sync incoming camera messages
  sync_.registerCallback(boost::bind(&Detector::Callback, this, _1, _2, _3));

  model_provider = std::make_unique<ModelProvider>(nh);
}

void Detector::Callback(const sensor_msgs::ImageConstPtr& image, const sensor_msgs::ImageConstPtr& depth_image,
                        const sensor_msgs::CameraInfoConstPtr& camera_info)
{
  std::lock_guard<std::mutex> lock(mutex_);
  image_ = image;
  depth_image_ = depth_image;
  camera_info_ = camera_info;
}

void Detector::ReconfigureCallback(const clf_object_recognition_cfg::Detect3dConfig& input, uint32_t /*level*/)
{
  ROS_INFO_NAMED("detector", "Reconfigure");
  config = input;
}

bool Detector::ServiceDetect3D(clf_object_recognition_msgs::Detect3D::Request& req,
                               clf_object_recognition_msgs::Detect3D::Response& res)
{
  sensor_msgs::Image img;
  sensor_msgs::Image depth;
  sensor_msgs::CameraInfo info;
  visualization_msgs::MarkerArray markers;
  int marker_id = 0;

  ROS_INFO_STREAM_NAMED("detector", "ServiceDetect3D() called " << req);

  // Fetch image data
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (image_ == nullptr)
    {
      ROS_ERROR_STREAM_NAMED("detector", "ServiceDetect3D() called: No rgb image provided");
      return false;
    }
    else if (depth_image_ == nullptr)
    {
      ROS_ERROR_STREAM_NAMED("detector", "ServiceDetect3D() called: No depth image provided");
      return false;
    }
    img = sensor_msgs::Image(*image_.get());
    depth = sensor_msgs::Image(*depth_image_.get());
    ROS_INFO_STREAM_NAMED("detector", "ServiceDetect3D() got images ");
  }

  clf_object_recognition_msgs::Detect2DImage param;
  // Call Detect2DImage service
  {
    param.request.image = img;
    param.request.min_conf = req.min_conf;
    auto ok = srv_detect_2d.call(param);
    if (!ok)
    {
      ROS_ERROR_STREAM_NAMED("detector", "cant call detections ");
      return false;
    }
    ROS_DEBUG_STREAM_NAMED("detector", "got " << param.response.detections.size() << " detections");
  }

  // transform base_link -> camera
  geometry_msgs::TransformStamped tf_base_to_cam;
  try {
    tf_base_to_cam = tf_buffer.lookupTransform(depth_image_->header.frame_id, "base_link", depth_image_->header.stamp);
  } catch (tf2::TransformException ex){
    ROS_WARN_STREAM_NAMED("detector", ex.what());
    // wait 1 sec to make sure buffer is updated
    // if somehow the yolox service call finished faster than joint update running with 100hz ?!
    ros::Duration(1.0).sleep();
    try {
      tf_base_to_cam = tf_buffer.lookupTransform(depth_image_->header.frame_id, "base_link", depth_image_->header.stamp);
    } catch(tf2::TransformException ex) {
      // fallback to latest available transform, somethings fucked anyhow
      tf_base_to_cam = tf_buffer.lookupTransform(depth_image_->header.frame_id, "base_link", ros::Time(0));
    }
    
  }

  for (auto& detection : param.response.detections)
  {
    vision_msgs::Detection3D d3d;
    // generate point cloud from incoming depth image for detection bounding box
    pointcloud_type::Ptr cloud_from_depth_image = cloud::fromDepthArea(detection.bbox, depth, *camera_info_);

    // TODO cleanup cloud a bit
    // cloud_from_depth_image = cloud::cleanupCloud(cloud_from_depth_image)

    // set source cloud only if using icp
    if(!req.skip_icp) {
      sensor_msgs::PointCloud2 pcl_msg;
      pcl::toROSMsg(*cloud_from_depth_image, pcl_msg);
      d3d.source_cloud = pcl_msg;
    }

    Eigen::Vector4d centroid = Eigen::Vector4d::Random();
    auto centroid_size = pcl::compute3DCentroid(*cloud_from_depth_image, centroid);

    pointcloud_type::Ptr centroid_filtered(new pointcloud_type());
    pcl::CropBox<point_type> box_filter;
    box_filter.setTranslation(Eigen::Vector3f(centroid[0], centroid[1], centroid[2]));
    box_filter.setMin(Eigen::Vector4f(-0.2, -0.2, -0.2, 1.0));
    box_filter.setMax(Eigen::Vector4f(0.2, 0.2, 0.2, 1.0));
    box_filter.setInputCloud(cloud_from_depth_image);
    box_filter.filter(*centroid_filtered);

    centroid_size = pcl::compute3DCentroid(*centroid_filtered, centroid);

    geometry_msgs::Pose center;
    // initial rotation with z aligned to world and pose at centroid
    center.orientation = tf_base_to_cam.transform.rotation;

    // verify centroid
    {
      if (centroid_size == 0)
      {
        ROS_ERROR_STREAM_NAMED("detector", "      centroid is invalid");
      }
      else
      {
        center.position.x = centroid[0];
        center.position.y = centroid[1];
        center.position.z = centroid[2];
      }

      if (center.position.x != center.position.x || center.position.y != center.position.y ||
          center.position.z != center.position.z)
      {
        ROS_DEBUG_STREAM_NAMED("detector", "      center NAN ");
        center.position.x = 0.1;
        center.position.y = 0.1;
        center.position.z = 0.1;
      }

      ROS_DEBUG_STREAM_NAMED("detector", "      center at " << center.position.x << ", " << center.position.y << ", "
                                                            << center.position.z);
    }  // verify centroid

    d3d.header = detection.header;
    d3d.bbox.center = center;
    d3d.bbox.size.x = 0.1;
    d3d.bbox.size.y = 0.1;
    d3d.bbox.size.z = 0.1;

    if(req.skip_icp) {
      for (auto hypo : detection.results)
      {
        ROS_DEBUG_STREAM_NAMED("detector", "  - hypo " << hypo.id);

        vision_msgs::ObjectHypothesisWithPose hyp = hypo;
        hyp.pose.pose = center;

        d3d.results.push_back(hyp);
      }
      res.detections.push_back(d3d);

    } else {
      Eigen::Matrix4f initial_guess;
      // initial guess
      {
        Eigen::Affine3d center_3d;
        tf2::fromMsg(center, center_3d);

        // TODO shift down 10cm in base_link frame
        Eigen::Affine3d t(Eigen::Translation3d(Eigen::Vector3d(0,0,-0.1)));
        t = tf2::transformToEigen(tf_base_to_cam).rotation() * t;
        const Eigen::IOFormat fmt(2, Eigen::DontAlignCols, "\t", " ", "", "", "", "");
        ROS_DEBUG_STREAM_NAMED("detector", "  - t " << t.matrix().format(fmt));
        auto t1 = center_3d.translation();
        auto t2 = t.translation();
        auto t3 = t1 + t2;
        center_3d.translation() = t3;

        Eigen::Matrix4d center_4d = center_3d.matrix();
        initial_guess = center_4d.cast<float>();
      }
    

      for (auto hypo : detection.results)
      {
        ROS_DEBUG_STREAM_NAMED("detector", "  - hypo " << hypo.id);

        vision_msgs::ObjectHypothesisWithPose hyp = hypo;

        auto model = model_provider->IDtoModel(hyp.id);
        auto model_path_dae = model_provider->GetModelPath(model);

        auto sampled = cloud::loadPointcloud(model_path_dae);
        std::shared_ptr<pcl::Registration<point_type, point_type>> reg;

        if(config.matcher == clf_object_recognition_cfg::Detect3d_gicp) {
          ROS_INFO_STREAM_NAMED("detector", "    running GICP...");
          auto gicp = std::make_shared<pcl::GeneralizedIterativeClosestPoint<point_type, point_type>>();

          gicp->setUseReciprocalCorrespondences(config.icp_use_reciprocal_correspondence);
          // setEuclideanFitnessEpsilon

          // setRotationEpsilon
          gicp->setCorrespondenceRandomness(config.gicp_correspondence_randomness);
          gicp->setMaximumOptimizerIterations(config.gicp_maximum_optimizer_iterations);
          // setTranslationGradientTolerance
          // setRotationGradientTolerance 

          reg = gicp;
        } else {
          ROS_INFO_STREAM_NAMED("detector", "    running ICP...");
          auto icp = std::make_shared<pcl::IterativeClosestPoint<point_type, point_type>>();

          icp->setUseReciprocalCorrespondences(config.icp_use_reciprocal_correspondence);
          // setEuclideanFitnessEpsilon

          reg = icp;
        }

        reg->setMaximumIterations(config.registration_maximum_iterations);
        reg->setRANSACIterations(config.registration_ransac_iterations);
        reg->setRANSACOutlierRejectionThreshold(config.registration_ransac_outlier_rejection_threshold);
        reg->setMaxCorrespondenceDistance(config.registration_max_correspondence_dist);
        reg->setTransformationEpsilon(config.registration_transformation_epsilon);
        reg->setTransformationRotationEpsilon(config.registration_transformation_rotation_epsilon);
      
        reg->setInputSource(sampled);
        reg->setInputTarget(cloud_from_depth_image);
        pointcloud_type final_point_cloud;
      
        reg->align(final_point_cloud, initial_guess);  
        ROS_INFO_STREAM_NAMED("detector", "    has converged: " << reg->hasConverged());

        Eigen::Matrix4f icp_transform = reg->getFinalTransformation();
        Eigen::Affine3f affine(icp_transform);

        geometry_msgs::Transform tf_msg;
        tf::transformEigenToMsg(affine.cast<double>(), tf_msg);

        ROS_DEBUG_STREAM_NAMED("detector", "    object at " << tf_msg.translation.x << ", " << tf_msg.translation.y
                                                              << ", " << tf_msg.translation.z);
        hyp.pose.pose.orientation = tf_msg.rotation;
        hyp.pose.pose.position.x = tf_msg.translation.x;
        hyp.pose.pose.position.y = tf_msg.translation.y;
        hyp.pose.pose.position.z = tf_msg.translation.z;

        pcl::transformPointCloud(*sampled, *sampled, initial_guess);
        sensor_msgs::PointCloud2 pcl_msg2;
        pcl::toROSMsg(*sampled, pcl_msg2);
        pcl_msg2.header = depth.header;
        pub_cloud.publish(pcl_msg2);

        if (config.publish_marker)
        {
          visualization_msgs::Marker marker;
          marker.type = visualization_msgs::Marker::MESH_RESOURCE;
          marker.id = marker_id++;
          marker.header = img.header;
          marker.scale.x = 1;
          marker.scale.y = 1;
          marker.scale.z = 1;
          marker.color.a = 0.5;
          marker.pose = hyp.pose.pose;
          marker.mesh_resource = model_path_dae;
          markers.markers.push_back(marker);
        }

        d3d.results.push_back(hyp);
      }
      res.detections.push_back(d3d);
    }
  }

  // Finished, publish debug messages

  if (config.publish_marker)
  {
    pub_marker.publish(markers);
  }

  if (config.publish_detections)
  {
    ROS_DEBUG_STREAM_NAMED("detector", "publishing detections ");
    vision_msgs::Detection3DArray a;
    for (auto b : res.detections)
    {
      a.header = b.header;
      a.detections.push_back(b);
    }
    pub_detections_3d.publish(a);
  }

  return true;
}
