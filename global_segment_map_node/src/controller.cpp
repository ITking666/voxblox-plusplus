// Copyright 2018 Margarita Grinvald, ASL, ETH Zurich, Switzerland

#include "voxblox_gsm/controller.h"

#include <stdlib.h>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <geometry_msgs/TransformStamped.h>
#include <global_segment_map/label_voxel.h>
#include <global_segment_map/utils/file_utils.h>
#include <glog/logging.h>
#include <minkindr_conversions/kindr_tf.h>

#include <minkindr_conversions/kindr_tf.h>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

#include <voxblox/core/common.h>
#include <voxblox/integrator/merge_integration.h>
#include <voxblox/utils/layer_utils.h>
#include <voxblox_ros/conversions.h>
#include <voxblox_ros/mesh_vis.h>

#ifdef APPROXMVBB_AVAILABLE
#include <ApproxMVBB/ComputeApproxMVBB.hpp>
#endif

#include "voxblox_gsm/conversions.h"

namespace voxblox {
namespace voxblox_gsm {

std::string classes[81] = {"BG",
                           "person",
                           "bicycle",
                           "car",
                           "motorcycle",
                           "airplane",
                           "bus",
                           "train",
                           "truck",
                           "boat",
                           "traffic light",
                           "fire hydrant",
                           "stop sign",
                           "parking meter",
                           "bench",
                           "bird",
                           "cat",
                           "dog",
                           "horse",
                           "sheep",
                           "cow",
                           "elephant",
                           "bear",
                           "zebra",
                           "giraffe",
                           "backpack",
                           "umbrella",
                           "handbag",
                           "tie",
                           "suitcase",
                           "frisbee",
                           "skis",
                           "snowboard",
                           "sports ball",
                           "kite",
                           "baseball bat",
                           "baseball glove",
                           "skateboard",
                           "surfboard",
                           "tennis racket",
                           "bottle",
                           "wine glass",
                           "cup",
                           "fork",
                           "knife",
                           "spoon",
                           "bowl",
                           "banana",
                           "apple",
                           "sandwich",
                           "orange",
                           "broccoli",
                           "carrot",
                           "hot dog",
                           "pizza",
                           "donut",
                           "cake",
                           "chair",
                           "couch",
                           "potted plant",
                           "bed",
                           "dining table",
                           "toilet",
                           "tv",
                           "laptop",
                           "mouse",
                           "remote",
                           "keyboard",
                           "cell phone",
                           "microwave",
                           "oven",
                           "toaster",
                           "sink",
                           "refrigerator",
                           "book",
                           "clock",
                           "vase",
                           "scissors",
                           "teddy bear",
                           "hair drier",
                           "toothbrush"};

Controller::Controller(ros::NodeHandle* node_handle_private)
    : node_handle_private_(node_handle_private),
      // Increased time limit for lookup in the past of tf messages
      // to give some slack to the pipeline and not lose any messages.
      integrated_frames_count_(0u),
      tf_listener_(ros::Duration(1000)),
      world_frame_("world"),
      camera_frame_(""),
      no_update_timeout_(0.0),
      publish_gsm_updates_(false),
      publish_scene_mesh_(false),
      received_first_message_(false),
      updated_mesh_(false),
      need_full_remesh_(false),
      enable_semantic_instance_segmentation_(true),
      compute_and_publish_bbox_(false),
      use_label_propagation_(true) {
  CHECK_NOTNULL(node_handle_private_);
  node_handle_private_->param<std::string>("world_frame_id", world_frame_,
                                           world_frame_);
  node_handle_private_->param<std::string>("camera_frame_id", camera_frame_,
                                           camera_frame_);

  // Workaround for OS X on mac mini not having specializations for float
  // for some reason.
  int voxels_per_side = map_config_.voxels_per_side;
  node_handle_private_->param<FloatingPoint>(
      "voxblox/voxel_size", map_config_.voxel_size, map_config_.voxel_size);
  node_handle_private_->param<int>("voxblox/voxels_per_side", voxels_per_side,
                                   voxels_per_side);
  if (!isPowerOfTwo(voxels_per_side)) {
    ROS_ERROR("voxels_per_side must be a power of 2, setting to default value");
    voxels_per_side = map_config_.voxels_per_side;
  }
  map_config_.voxels_per_side = voxels_per_side;

  map_.reset(new LabelTsdfMap(map_config_));

  // Determine TSDF integrator parameters.
  tsdf_integrator_config_.voxel_carving_enabled = false;
  tsdf_integrator_config_.allow_clear = true;
  FloatingPoint truncation_distance_factor = 5.0f;
  tsdf_integrator_config_.max_ray_length_m = 2.5f;

  node_handle_private_->param<bool>(
      "voxblox/voxel_carving_enabled",
      tsdf_integrator_config_.voxel_carving_enabled,
      tsdf_integrator_config_.voxel_carving_enabled);
  node_handle_private_->param<bool>("voxblox/allow_clear",
                                    tsdf_integrator_config_.allow_clear,
                                    tsdf_integrator_config_.allow_clear);
  node_handle_private_->param<FloatingPoint>(
      "voxblox/truncation_distance_factor", truncation_distance_factor,
      truncation_distance_factor);
  node_handle_private_->param<FloatingPoint>(
      "voxblox/min_ray_length_m", tsdf_integrator_config_.min_ray_length_m,
      tsdf_integrator_config_.min_ray_length_m);
  node_handle_private_->param<FloatingPoint>(
      "voxblox/max_ray_length_m", tsdf_integrator_config_.max_ray_length_m,
      tsdf_integrator_config_.max_ray_length_m);

  tsdf_integrator_config_.default_truncation_distance =
      map_config_.voxel_size * truncation_distance_factor;

  std::string method("merged");
  node_handle_private_->param<std::string>("method", method, method);
  if (method.compare("merged") == 0) {
    tsdf_integrator_config_.enable_anti_grazing = false;
  } else if (method.compare("merged_discard") == 0) {
    tsdf_integrator_config_.enable_anti_grazing = true;
  } else {
    tsdf_integrator_config_.enable_anti_grazing = false;
  }

  // Determine label integrator parameters.
  node_handle_private_->param<bool>(
      "pairwise_confidence_merging/enable_pairwise_confidence_merging",
      label_tsdf_integrator_config_.enable_pairwise_confidence_merging,
      label_tsdf_integrator_config_.enable_pairwise_confidence_merging);
  node_handle_private_->param<FloatingPoint>(
      "pairwise_confidence_merging/merging_min_overlap_ratio",
      label_tsdf_integrator_config_.merging_min_overlap_ratio,
      label_tsdf_integrator_config_.merging_min_overlap_ratio);
  node_handle_private_->param<int>(
      "pairwise_confidence_merging/merging_min_frame_count",
      label_tsdf_integrator_config_.merging_min_frame_count,
      label_tsdf_integrator_config_.merging_min_frame_count);

  node_handle_private_->param<bool>(
      "semantic_instance_segmentation/enable_semantic_instance_segmentation",
      label_tsdf_integrator_config_.enable_semantic_instance_segmentation,
      label_tsdf_integrator_config_.enable_semantic_instance_segmentation);

  enable_semantic_instance_segmentation_ =
      label_tsdf_integrator_config_.enable_semantic_instance_segmentation;

  node_handle_private_->param<int>(
      "object_database/max_segment_age",
      label_tsdf_integrator_config_.max_segment_age,
      label_tsdf_integrator_config_.max_segment_age);

  // TODO(margaritaG) : this is not used, remove?
  // std::string mesh_color_scheme("label");
  // node_handle_private_->param<std::string>(
  //     "meshing/mesh_color_scheme", mesh_color_scheme, mesh_color_scheme);
  // if (mesh_color_scheme.compare("label") == 0) {
  //   mesh_color_scheme_ = MeshLabelIntegrator::LabelColor;
  // } else if (mesh_color_scheme.compare("semantic_label") == 0) {
  //   mesh_color_scheme_ = MeshLabelIntegrator::SemanticColor;
  // } else if (mesh_color_scheme.compare("instance_label") == 0) {
  //   mesh_color_scheme_ = MeshLabelIntegrator::InstanceColor;
  // } else if (mesh_color_scheme.compare("geometric_instance_label") == 0) {
  //   mesh_color_scheme_ = MeshLabelIntegrator::GeometricInstanceColor;
  // } else if (mesh_color_scheme.compare("confidence") == 0) {
  //   mesh_color_scheme_ = MeshLabelIntegrator::ConfidenceColor;
  // } else {
  //   mesh_color_scheme_ = MeshLabelIntegrator::LabelColor;
  // }

  std::string class_task("coco80");
  node_handle_private_->param<std::string>(
      "semantic_instance_segmentation/class_task", class_task, class_task);
  if (class_task.compare("coco80") == 0) {
    label_tsdf_mesh_config_.class_task = SemanticColorMap::ClassTask ::kCoco80;
  } else if (class_task.compare("nyu13") == 0) {
    label_tsdf_mesh_config_.class_task = SemanticColorMap::ClassTask ::kNyu13;
  } else {
    label_tsdf_mesh_config_.class_task = SemanticColorMap::ClassTask::kCoco80;
  }

  integrator_.reset(new LabelTsdfIntegrator(
      tsdf_integrator_config_, label_tsdf_integrator_config_, map_.get()));

  mesh_label_layer_.reset(new MeshLayer(map_->block_size()));

  label_tsdf_mesh_config_.color_scheme =
      MeshLabelIntegrator::ColorScheme::kLabel;
  mesh_label_integrator_.reset(new MeshLabelIntegrator(
      mesh_config_, label_tsdf_mesh_config_, map_.get(),
      mesh_label_layer_.get(), all_semantic_labels_, &need_full_remesh_));

  if (enable_semantic_instance_segmentation_) {
    mesh_semantic_layer_.reset(new MeshLayer(map_->block_size()));
    mesh_instance_layer_.reset(new MeshLayer(map_->block_size()));
    mesh_merged_layer_.reset(new MeshLayer(map_->block_size()));

    label_tsdf_mesh_config_.color_scheme =
        MeshLabelIntegrator::ColorScheme::kSemantic;
    mesh_semantic_integrator_.reset(new MeshLabelIntegrator(
        mesh_config_, label_tsdf_mesh_config_, map_.get(),
        mesh_semantic_layer_.get(), all_semantic_labels_, &need_full_remesh_));
    label_tsdf_mesh_config_.color_scheme =
        MeshLabelIntegrator::ColorScheme::kInstance;
    mesh_instance_integrator_.reset(new MeshLabelIntegrator(
        mesh_config_, label_tsdf_mesh_config_, map_.get(),
        mesh_instance_layer_.get(), all_semantic_labels_, &need_full_remesh_));
    label_tsdf_mesh_config_.color_scheme =
        MeshLabelIntegrator::ColorScheme::kMerged;
    mesh_merged_integrator_.reset(new MeshLabelIntegrator(
        mesh_config_, label_tsdf_mesh_config_, map_.get(),
        mesh_merged_layer_.get(), all_semantic_labels_, &need_full_remesh_));
  }

  // Visualization settings.
  bool visualize = false;
  node_handle_private_->param<bool>("meshing/visualize", visualize, visualize);
  if (visualize) {
    std::vector<std::shared_ptr<MeshLayer>> mesh_layers;
    mesh_layers.push_back(mesh_label_layer_);

    if (enable_semantic_instance_segmentation_) {
      mesh_layers.push_back(mesh_instance_layer_);
      mesh_layers.push_back(mesh_semantic_layer_);
      mesh_layers.push_back(mesh_merged_layer_);
    }
    visualizer_ =
        new Visualizer(mesh_layers, &updated_mesh_, &updated_mesh_mutex_);
    viz_thread_ = std::thread(&Visualizer::visualizeMesh, visualizer_);
  }

  node_handle_private_->param<bool>("meshing/publish_segment_mesh",
                                    publish_segment_mesh_,
                                    publish_segment_mesh_);
  node_handle_private_->param<bool>("meshing/publish_scene_mesh",
                                    publish_scene_mesh_, publish_scene_mesh_);
  node_handle_private_->param<bool>("meshing/compute_and_publish_bbox",
                                    compute_and_publish_bbox_,
                                    compute_and_publish_bbox_);
#ifndef APPROXMVBB_AVAILABLE
  if (compute_and_publish_bbox_) {
    ROS_WARN_STREAM(
        "ApproxMVBB is not available and therefore bounding box "
        "functionality "
        "is disabled.");
  }
  compute_and_publish_bbox_ = false;
#endif

  node_handle_private_->param<bool>(
      "use_label_propagation", use_label_propagation_, use_label_propagation_);

  // If set, use a timer to progressively update the mesh.
  double update_mesh_every_n_sec = 0.0;
  node_handle_private_->param<double>("meshing/update_mesh_every_n_sec",
                                      update_mesh_every_n_sec,
                                      update_mesh_every_n_sec);

  if (update_mesh_every_n_sec > 0.0) {
    update_mesh_timer_ = node_handle_private_->createTimer(
        ros::Duration(update_mesh_every_n_sec), &Controller::updateMeshEvent,
        this);
  }

  node_handle_private_->param<std::string>("meshing/mesh_filename",
                                           mesh_filename_, mesh_filename_);

  node_handle_private_->param<bool>("object_database/publish_gsm_updates",
                                    publish_gsm_updates_, publish_gsm_updates_);

  node_handle_private_->param<double>("object_database/no_update_timeout",
                                      no_update_timeout_, no_update_timeout_);
}

Controller::~Controller() { viz_thread_.join(); }

void Controller::subscribeSegmentPointCloudTopic(
    ros::Subscriber* segment_point_cloud_sub) {
  CHECK_NOTNULL(segment_point_cloud_sub);
  std::string segment_point_cloud_topic =
      "/depth_segmentation_node/object_segment";
  node_handle_private_->param<std::string>("segment_point_cloud_topic",
                                           segment_point_cloud_topic,
                                           segment_point_cloud_topic);
  // TODO (margaritaG): make this a param once segments of a frame are
  // refactored to be received as one single message.
  // Large queue size to give slack to the
  // pipeline and not lose any messages.
  *segment_point_cloud_sub = node_handle_private_->subscribe(
      segment_point_cloud_topic, 6000, &Controller::segmentPointCloudCallback,
      this);
}

void Controller::advertiseSegmentGsmUpdateTopic(
    ros::Publisher* segment_gsm_update_pub) {
  CHECK_NOTNULL(segment_gsm_update_pub);
  std::string segment_gsm_update_topic = "gsm_update";
  node_handle_private_->param<std::string>("segment_gsm_update_topic",
                                           segment_gsm_update_topic,
                                           segment_gsm_update_topic);
  // TODO(ff): Reduce this value, once we know some reasonable limit.
  constexpr int kGsmUpdateQueueSize = 2000;

  *segment_gsm_update_pub =
      node_handle_private_->advertise<modelify_msgs::GsmUpdate>(
          segment_gsm_update_topic, kGsmUpdateQueueSize, true);
  segment_gsm_update_pub_ = segment_gsm_update_pub;
}

void Controller::advertiseSceneGsmUpdateTopic(
    ros::Publisher* scene_gsm_update_pub) {
  CHECK_NOTNULL(scene_gsm_update_pub);
  std::string scene_gsm_update_topic = "scene";
  node_handle_private_->param<std::string>(
      "scene_gsm_update_topic", scene_gsm_update_topic, scene_gsm_update_topic);
  constexpr int kGsmSceneQueueSize = 1;

  *scene_gsm_update_pub =
      node_handle_private_->advertise<modelify_msgs::GsmUpdate>(
          scene_gsm_update_topic, kGsmSceneQueueSize, true);
  scene_gsm_update_pub_ = scene_gsm_update_pub;
}

void Controller::advertiseSegmentMeshTopic(ros::Publisher* segment_mesh_pub) {
  CHECK_NOTNULL(segment_mesh_pub);
  *segment_mesh_pub = node_handle_private_->advertise<voxblox_msgs::Mesh>(
      "segment_mesh", 1, true);

  segment_mesh_pub_ = segment_mesh_pub;
}

void Controller::advertiseSceneMeshTopic(ros::Publisher* scene_mesh_pub) {
  CHECK_NOTNULL(scene_mesh_pub);
  *scene_mesh_pub =
      node_handle_private_->advertise<voxblox_msgs::Mesh>("mesh", 1, true);

  scene_mesh_pub_ = scene_mesh_pub;
}

void Controller::advertiseBboxTopic(ros::Publisher* bbox_pub) {
  CHECK_NOTNULL(bbox_pub);
  *bbox_pub = node_handle_private_->advertise<visualization_msgs::Marker>(
      "bbox", 1, true);

  bbox_pub_ = bbox_pub;
}

void Controller::advertisePublishSceneService(
    ros::ServiceServer* publish_scene_srv) {
  CHECK_NOTNULL(publish_scene_srv);
  static const std::string kAdvertisePublishSceneServiceName = "publish_scene";
  *publish_scene_srv = node_handle_private_->advertiseService(
      kAdvertisePublishSceneServiceName, &Controller::publishSceneCallback,
      this);
}

void Controller::validateMergedObjectService(
    ros::ServiceServer* validate_merged_object_srv) {
  CHECK_NOTNULL(validate_merged_object_srv);
  static const std::string kValidateMergedObjectTopicRosParam =
      "validate_merged_object";
  std::string validate_merged_object_topic = "validate_merged_object";
  node_handle_private_->param<std::string>(kValidateMergedObjectTopicRosParam,
                                           validate_merged_object_topic,
                                           validate_merged_object_topic);

  *validate_merged_object_srv = node_handle_private_->advertiseService(
      validate_merged_object_topic, &Controller::validateMergedObjectCallback,
      this);
}

void Controller::advertiseGenerateMeshService(
    ros::ServiceServer* generate_mesh_srv) {
  CHECK_NOTNULL(generate_mesh_srv);
  *generate_mesh_srv = node_handle_private_->advertiseService(
      "generate_mesh", &Controller::generateMeshCallback, this);
}

void Controller::advertiseExtractSegmentsService(
    ros::ServiceServer* extract_segments_srv) {
  CHECK_NOTNULL(extract_segments_srv);
  *extract_segments_srv = node_handle_private_->advertiseService(
      "extract_segments", &Controller::extractSegmentsCallback, this);
}

void Controller::advertiseExtractInstancesService(
    ros::ServiceServer* extract_instances_srv) {
  CHECK_NOTNULL(extract_instances_srv);
  *extract_instances_srv = node_handle_private_->advertiseService(
      "extract_instances", &Controller::extractInstancesCallback, this);
}

void Controller::segmentPointCloudCallback(
    const sensor_msgs::PointCloud2::Ptr& segment_point_cloud_msg) {
  // Message timestamps are used to detect when all
  // segment messages from a certain frame have arrived.
  // Since segments from the same frame all have the same timestamp,
  // the start of a new frame is detected when the message timestamp changes.
  // TODO(grinvalm): need additional check for the last frame to be
  // integrated.
  if (received_first_message_ &&
      last_segment_msg_timestamp_ != segment_point_cloud_msg->header.stamp) {
    ROS_INFO_STREAM("Timings: " << std::endl << timing::Timing::Print());
    timing::Timer ptcloud_timer("label_propagation");

    ROS_INFO("Integrating frame n.%zu, timestamp of frame: %f",
             ++integrated_frames_count_,
             segment_point_cloud_msg->header.stamp.toSec());

    if (use_label_propagation_) {
      ros::WallTime start = ros::WallTime::now();

      integrator_->decideLabelPointClouds(&segments_to_integrate_,
                                          &segment_label_candidates,
                                          &segment_merge_candidates_);

      ros::WallTime end = ros::WallTime::now();
      ROS_INFO("Decided labels for %lu pointclouds in %f seconds.",
               segments_to_integrate_.size(), (end - start).toSec());
    }

    constexpr bool kIsFreespacePointcloud = false;

    ros::WallTime start = ros::WallTime::now();
    {
      timing::Timer integrate_timer("integrate_frame_pointclouds");
      std::lock_guard<std::mutex> updatedMeshLock(updated_mesh_mutex_);
      for (const auto& segment : segments_to_integrate_) {
        integrator_->integratePointCloud(segment->T_G_C_, segment->points_C_,
                                         segment->colors_, segment->label_,
                                         kIsFreespacePointcloud);
      }
      integrate_timer.Stop();
    }

    ros::WallTime end = ros::WallTime::now();
    ROS_INFO(
        "Integrated %lu pointclouds in %f secs, have %lu tsdf "
        "and %lu label blocks.",
        segments_to_integrate_.size(), (end - start).toSec(),
        map_->getTsdfLayerPtr()->getNumberOfAllocatedBlocks(),
        map_->getLabelLayerPtr()->getNumberOfAllocatedBlocks());

    start = ros::WallTime::now();

    integrator_->mergeLabels(&merges_to_publish_);

    integrator_->getLabelsToPublish(&segment_labels_to_publish_);

    end = ros::WallTime::now();
    ROS_INFO(
        "Merged segments and fetched the ones to publish in %f "
        "seconds.",
        (end - start).toSec());
    start = ros::WallTime::now();

    segment_merge_candidates_.clear();
    segment_label_candidates.clear();
    for (Segment* segment : segments_to_integrate_) {
      delete segment;
    }
    segments_to_integrate_.clear();

    end = ros::WallTime::now();
    ROS_INFO("Cleared candidates and memory in %f seconds.",
             (end - start).toSec());

    // if (publish_gsm_updates_ && publishObjects()) {
    //   publishScene();
    // }
  }
  received_first_message_ = true;
  last_update_received_ = ros::Time::now();
  last_segment_msg_timestamp_ = segment_point_cloud_msg->header.stamp;

  // Look up transform from camera frame to world frame.
  Transformation T_G_C;
  std::string from_frame = camera_frame_;
  if (camera_frame_.empty()) {
    from_frame = segment_point_cloud_msg->header.frame_id;
  }
  if (lookupTransform(from_frame, world_frame_,
                      segment_point_cloud_msg->header.stamp, &T_G_C)) {
    // Convert the PCL pointcloud into voxblox format.
    // Horrible hack fix to fix color parsing colors in PCL.
    for (size_t d = 0; d < segment_point_cloud_msg->fields.size(); ++d) {
      if (segment_point_cloud_msg->fields[d].name == std::string("rgb")) {
        segment_point_cloud_msg->fields[d].datatype =
            sensor_msgs::PointField::FLOAT32;
      }
    }
    timing::Timer ptcloud_timer("ptcloud_preprocess");

    Segment* segment;
    if (enable_semantic_instance_segmentation_) {
      pcl::PointCloud<voxblox::PointSemanticInstanceType>
          point_cloud_semantic_instance;
      pcl::fromROSMsg(*segment_point_cloud_msg, point_cloud_semantic_instance);
      segment = new Segment(point_cloud_semantic_instance, T_G_C);
    } else if (use_label_propagation_) {
      pcl::PointCloud<voxblox::PointLabelType> point_cloud_label;
      pcl::fromROSMsg(*segment_point_cloud_msg, point_cloud_label);
      segment = new Segment(point_cloud_label, T_G_C);
    } else {
      pcl::PointCloud<voxblox::PointType> point_cloud;
      pcl::fromROSMsg(*segment_point_cloud_msg, point_cloud);
      segment = new Segment(point_cloud, T_G_C);
    }
    segments_to_integrate_.push_back(segment);

    // if (use_label_propagation_) {
    //   fillSegmentWithData(segment_point_cloud_msg, segment);
    // } else {
    //   fillSegmentWithData<PointSurfelLabel>(segment_point_cloud_msg,
    //   segment);
    // }
    ptcloud_timer.Stop();

    timing::Timer label_candidates_timer("compute_label_candidates");

    if (use_label_propagation_) {
      ros::WallTime start = ros::WallTime::now();
      integrator_->computeSegmentLabelCandidates(
          segment, &segment_label_candidates, &segment_merge_candidates_);

      ros::WallTime end = ros::WallTime::now();
      ROS_INFO(
          "Computed label candidates for a pointcloud of size %lu in %f "
          "seconds.",
          segment->points_C_.size(), (end - start).toSec());
    }

    ROS_INFO_STREAM("Timings: " << std::endl << timing::Timing::Print());
  }
}

bool Controller::publishSceneCallback(std_srvs::SetBool::Request& request,
                                      std_srvs::SetBool::Response& response) {
  bool save_scene_mesh = request.data;
  if (save_scene_mesh) {
    constexpr bool kClearMesh = true;
    generateMesh(kClearMesh);
  }
  publishScene();
  constexpr bool kPublishAllSegments = true;
  publishObjects(kPublishAllSegments);
  return true;
}

bool Controller::validateMergedObjectCallback(
    modelify_msgs::ValidateMergedObject::Request& request,
    modelify_msgs::ValidateMergedObject::Response& response) {
  typedef TsdfVoxel TsdfVoxelType;
  typedef Layer<TsdfVoxelType> TsdfLayer;
  // TODO(ff): Do the following afterwards in modelify.
  // - Check if merged object agrees with whole map (at all poses).
  // - If it doesn't agree at all poses try the merging again with the
  // reduced set of objects.
  // - Optionally put the one that doesn't agree with
  // the others in a list not to merge with the other merged ones

  // Extract TSDF layer of merged object.
  std::shared_ptr<TsdfLayer> merged_object_layer_O;
  CHECK(deserializeMsgToLayer(request.gsm_update.object.tsdf_layer,
                              merged_object_layer_O.get()))
      << "Deserializing of TSDF layer from merged object message failed.";

  // Extract transformations.
  std::vector<Transformation> transforms_W_O;
  voxblox_gsm::transformMsgs2Transformations(
      request.gsm_update.object.transforms, &transforms_W_O);

  const utils::VoxelEvaluationMode voxel_evaluation_mode =
      utils::VoxelEvaluationMode::kEvaluateAllVoxels;

  std::vector<utils::VoxelEvaluationDetails> voxel_evaluation_details_vector;

  evaluateLayerRmseAtPoses<TsdfVoxelType>(
      voxel_evaluation_mode, map_->getTsdfLayer(),
      *(merged_object_layer_O.get()), transforms_W_O,
      &voxel_evaluation_details_vector);

  voxblox_gsm::voxelEvaluationDetails2VoxelEvaluationDetailsMsg(
      voxel_evaluation_details_vector, &response.voxel_evaluation_details);
  return true;
}

bool Controller::generateMeshCallback(
    std_srvs::Empty::Request& request,
    std_srvs::Empty::Response& response) {  // NOLINT
  constexpr bool kClearMesh = true;
  generateMesh(kClearMesh);
  return true;
}

bool Controller::extractSegmentsCallback(std_srvs::Empty::Request& request,
                                         std_srvs::Empty::Response& response) {
  // Get list of all labels in the map.
  Labels labels = map_->getLabelList();
  static constexpr bool kConnectedMesh = false;

  std::unordered_map<Label, LayerPair> label_to_layers;
  // Extract the TSDF and label layers corresponding to a segment.
  constexpr bool kLabelsListIsComplete = false;
  map_->extractSegmentLayers(labels, &label_to_layers, kLabelsListIsComplete);

  for (Label label : labels) {
    auto it = label_to_layers.find(label);
    CHECK(it != label_to_layers.end())
        << "Layers for label " << label << " could not be extracted.";

    const Layer<TsdfVoxel>& segment_tsdf_layer = it->second.first;
    const Layer<LabelVoxel>& segment_label_layer = it->second.second;

    CHECK_EQ(voxblox::file_utils::makePath("gsm_segments", 0777), 0);

    std::string mesh_filename =
        "gsm_segments/gsm_segment_mesh_label_" + std::to_string(label) + ".ply";

    bool success = voxblox::io::outputLayerAsPly(
        segment_tsdf_layer, mesh_filename,
        voxblox::io::PlyOutputTypes::kSdfIsosurface);

    if (success) {
      ROS_INFO("Output segment file as PLY: %s", mesh_filename.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename.c_str());
    }
  }
  return true;
}

bool Controller::extractInstancesCallback(std_srvs::Empty::Request& request,
                                          std_srvs::Empty::Response& response) {
  // Get list of all instances in the map.
  InstanceLabels instance_labels = map_->getInstanceList();
  static constexpr bool kConnectedMesh = false;

  std::unordered_map<InstanceLabel, LayerPair> instance_label_to_layers;
  // Extract the TSDF and label layers corresponding to a segment.
  constexpr bool kLabelsListIsComplete = true;
  map_->extractInstanceLayers(instance_labels, &instance_label_to_layers);

  for (InstanceLabel instance_label : instance_labels) {
    auto it = instance_label_to_layers.find(instance_label);
    CHECK(it != instance_label_to_layers.end())
        << "Layers for instance label " << instance_label
        << " could not be extracted.";

    const Layer<TsdfVoxel>& segment_tsdf_layer = it->second.first;
    const Layer<LabelVoxel>& segment_label_layer = it->second.second;

    CHECK_EQ(voxblox::file_utils::makePath("gsm_instances", 0777), 0);

    std::string mesh_filename = "gsm_instances/gsm_segment_mesh_label_" +
                                std::to_string(instance_label) + ".ply";

    bool success = voxblox::io::outputLayerAsPly(
        segment_tsdf_layer, mesh_filename,
        voxblox::io::PlyOutputTypes::kSdfIsosurface);

    if (success) {
      ROS_INFO("Output segment file as PLY: %s", mesh_filename.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename.c_str());
    }
  }
  return true;
}

bool Controller::lookupTransform(const std::string& from_frame,
                                 const std::string& to_frame,
                                 const ros::Time& timestamp,
                                 Transformation* transform) {
  tf::StampedTransform tf_transform;
  ros::Time time_to_lookup = timestamp;

  // If this transform isn't possible at the time, then try to just look
  // up the latest (this is to work with bag files and static transform
  // publisher, etc).
  if (!tf_listener_.canTransform(to_frame, from_frame, time_to_lookup)) {
    time_to_lookup = ros::Time(0);
    LOG(ERROR) << "Using latest TF transform instead of timestamp match.";
    return false;
  }

  try {
    tf_listener_.lookupTransform(to_frame, from_frame, time_to_lookup,
                                 tf_transform);
  } catch (tf::TransformException& ex) {
    LOG(ERROR) << "Error getting TF transform from sensor data: " << ex.what();
    return false;
  }

  tf::transformTFToKindr(tf_transform, transform);
  return true;
}

// TODO(ff): Create this somewhere:
// void serializeGsmAsMsg(const map&, const label&, const parent_labels&,
// msg*);

bool Controller::publishObjects(const bool publish_all) {
  CHECK_NOTNULL(segment_gsm_update_pub_);
  bool published_segment_label = false;
  std::vector<Label> labels_to_publish;
  getLabelsToPublish(publish_all, &labels_to_publish);

  std::unordered_map<Label, LayerPair> label_to_layers;
  ros::Time start = ros::Time::now();
  map_->extractSegmentLayers(labels_to_publish, &label_to_layers, publish_all);
  ros::Time stop = ros::Time::now();
  ros::Duration duration = stop - start;
  LOG(INFO) << "Extracting segment layers took " << duration.toSec() << "s";

  for (const Label& label : labels_to_publish) {
    auto it = label_to_layers.find(label);
    CHECK(it != label_to_layers.end())
        << "Layers for " << label << " could not be extracted.";

    Layer<TsdfVoxel>& tsdf_layer = it->second.first;
    Layer<LabelVoxel>& label_layer = it->second.second;

    // TODO(ff): Check what a reasonable size is for this.
    constexpr size_t kMinNumberOfAllocatedBlocksToPublish = 10u;
    if (tsdf_layer.getNumberOfAllocatedBlocks() <
        kMinNumberOfAllocatedBlocksToPublish) {
      continue;
    }

    // Convert to origin and extract translation.
    Point origin_shifted_tsdf_layer_W;
    utils::centerBlocksOfLayer<TsdfVoxel>(&tsdf_layer,
                                          &origin_shifted_tsdf_layer_W);
    // TODO(ff): If this is time consuming we can omit this step.
    Point origin_shifted_label_layer_W;
    utils::centerBlocksOfLayer<LabelVoxel>(&label_layer,
                                           &origin_shifted_label_layer_W);
    CHECK_EQ(origin_shifted_tsdf_layer_W, origin_shifted_label_layer_W);

    // Extract surfel cloud from layer.
    MeshIntegratorConfig mesh_config;
    node_handle_private_->param<float>("mesh_config/min_weight",
                                       mesh_config.min_weight,
                                       mesh_config.min_weight);
    pcl::PointCloud<pcl::PointSurfel>::Ptr surfel_cloud(
        new pcl::PointCloud<pcl::PointSurfel>());
    convertVoxelGridToPointCloud(tsdf_layer, mesh_config, surfel_cloud.get());

    if (surfel_cloud->empty()) {
      LOG(WARNING) << tsdf_layer.getNumberOfAllocatedBlocks()
                   << " blocks didn't produce a surface.";
      LOG(WARNING) << "Labelled segment does not contain enough data to "
                      "extract a surface -> skipping!";
      continue;
    }

    modelify_msgs::GsmUpdate gsm_update_msg;
    constexpr bool kSerializeOnlyUpdated = false;
    gsm_update_msg.header.stamp = last_segment_msg_timestamp_;
    gsm_update_msg.header.frame_id = world_frame_;
    gsm_update_msg.is_scene = false;
    serializeLayerAsMsg<TsdfVoxel>(tsdf_layer, kSerializeOnlyUpdated,
                                   &gsm_update_msg.object.tsdf_layer);
    // TODO(ff): Make sure this works also, there is no LabelVoxel in voxblox
    // yet, hence it doesn't work.
    serializeLayerAsMsg<LabelVoxel>(label_layer, kSerializeOnlyUpdated,
                                    &gsm_update_msg.object.label_layer);

    gsm_update_msg.object.label = label;
    gsm_update_msg.object.semantic_label =
        map_->getSemanticInstanceLabelFusionPtr()->getSemanticLabel(label);
    gsm_update_msg.old_labels.clear();
    geometry_msgs::Transform transform;
    transform.translation.x = origin_shifted_tsdf_layer_W[0];
    transform.translation.y = origin_shifted_tsdf_layer_W[1];
    transform.translation.z = origin_shifted_tsdf_layer_W[2];
    transform.rotation.w = 1.;
    transform.rotation.x = 0.;
    transform.rotation.y = 0.;
    transform.rotation.z = 0.;
    gsm_update_msg.object.transforms.clear();
    gsm_update_msg.object.transforms.push_back(transform);
    gsm_update_msg.object.surfel_cloud.header.frame_id = world_frame_;
    pcl::toROSMsg(*surfel_cloud, gsm_update_msg.object.surfel_cloud);

    if (all_published_segments_.find(label) != all_published_segments_.end()) {
      // Segment previously published, sending update message.
      gsm_update_msg.old_labels.push_back(label);
    } else {
      // Segment never previously published, sending first type of message.
    }
    auto merged_label_it = merges_to_publish_.find(label);
    if (merged_label_it != merges_to_publish_.end()) {
      for (Label merged_label : merged_label_it->second) {
        if (all_published_segments_.find(merged_label) !=
            all_published_segments_.end()) {
          gsm_update_msg.old_labels.push_back(merged_label);
        }
      }
      merges_to_publish_.erase(merged_label_it);
    }

    if (compute_and_publish_bbox_) {
      Eigen::Vector3f bbox_translation;
      Eigen::Quaternionf bbox_quaternion;
      Eigen::Vector3f bbox_size;
      computeAlignedBoundingBox(surfel_cloud, &bbox_translation,
                                &bbox_quaternion, &bbox_size);

      gsm_update_msg.object.bbox.pose.position.x =
          origin_shifted_tsdf_layer_W[0] + bbox_translation(0);
      gsm_update_msg.object.bbox.pose.position.y =
          origin_shifted_tsdf_layer_W[1] + bbox_translation(1);
      gsm_update_msg.object.bbox.pose.position.z =
          origin_shifted_tsdf_layer_W[2] + bbox_translation(2);
      gsm_update_msg.object.bbox.pose.orientation.x = bbox_quaternion.x();
      gsm_update_msg.object.bbox.pose.orientation.y = bbox_quaternion.y();
      gsm_update_msg.object.bbox.pose.orientation.z = bbox_quaternion.z();
      gsm_update_msg.object.bbox.pose.orientation.w = bbox_quaternion.w();
      gsm_update_msg.object.bbox.dimensions.x = bbox_size(0);
      gsm_update_msg.object.bbox.dimensions.y = bbox_size(1);
      gsm_update_msg.object.bbox.dimensions.z = bbox_size(2);

      visualization_msgs::Marker marker;
      marker.header.frame_id = world_frame_;
      marker.header.stamp = ros::Time();
      marker.id = gsm_update_msg.object.label;
      marker.type = visualization_msgs::Marker::CUBE;
      marker.action = visualization_msgs::Marker::ADD;
      marker.pose.position.x =
          origin_shifted_tsdf_layer_W[0] + bbox_translation(0);
      marker.pose.position.y =
          origin_shifted_tsdf_layer_W[1] + bbox_translation(1);
      marker.pose.position.z =
          origin_shifted_tsdf_layer_W[2] + bbox_translation(2);
      marker.pose.orientation = gsm_update_msg.object.bbox.pose.orientation;
      marker.scale = gsm_update_msg.object.bbox.dimensions;
      marker.color.a = 0.3;
      marker.color.r = 0.0;
      marker.color.g = 1.0;
      marker.color.b = 0.0;
      marker.lifetime = ros::Duration();

      bbox_pub_->publish(marker);

      geometry_msgs::TransformStamped bbox_tf;
      bbox_tf.header = gsm_update_msg.header;
      bbox_tf.child_frame_id = std::to_string(gsm_update_msg.object.label);
      bbox_tf.transform.translation.x = marker.pose.position.x;
      bbox_tf.transform.translation.y = marker.pose.position.y;
      bbox_tf.transform.translation.z = marker.pose.position.z;
      bbox_tf.transform.rotation = marker.pose.orientation;

      tf_broadcaster_.sendTransform(bbox_tf);
    }

    publishGsmUpdate(*segment_gsm_update_pub_, &gsm_update_msg);

    if (publish_segment_mesh_) {
      // Generate mesh for visualization purposes.
      std::shared_ptr<MeshLayer> mesh_layer;
      mesh_layer.reset(new MeshLayer(tsdf_layer.block_size()));
      // mesh_layer.reset(new MeshLayer(map_->block_size()));
      MeshLabelIntegrator::LabelTsdfConfig label_tsdf_mesh_config;
      label_tsdf_mesh_config.color_scheme =
          MeshLabelIntegrator::ColorScheme::kColor;
      MeshLabelIntegrator mesh_integrator(mesh_config_, label_tsdf_mesh_config,
                                          tsdf_layer, label_layer,
                                          mesh_layer.get());
      constexpr bool only_mesh_updated_blocks = false;
      constexpr bool clear_updated_flag = false;
      mesh_integrator.generateMesh(only_mesh_updated_blocks,
                                   clear_updated_flag);

      voxblox_msgs::Mesh segment_mesh_msg;
      generateVoxbloxMeshMsg(mesh_layer, ColorMode::kColor, &segment_mesh_msg);
      segment_mesh_msg.header.frame_id = world_frame_;
      segment_mesh_pub_->publish(segment_mesh_msg);
    }
    all_published_segments_.insert(label);
    published_segment_label = true;
  }
  segment_labels_to_publish_.clear();
  return published_segment_label;
}

void Controller::publishScene() {
  CHECK_NOTNULL(scene_gsm_update_pub_);
  modelify_msgs::GsmUpdate gsm_update_msg;

  gsm_update_msg.header.stamp = last_segment_msg_timestamp_;
  gsm_update_msg.header.frame_id = world_frame_;

  constexpr bool kSerializeOnlyUpdated = false;
  serializeLayerAsMsg<TsdfVoxel>(map_->getTsdfLayer(), kSerializeOnlyUpdated,
                                 &gsm_update_msg.object.tsdf_layer);
  // TODO(ff): Make sure this works also, there is no LabelVoxel in voxblox
  // yet, hence it doesn't work.
  serializeLayerAsMsg<LabelVoxel>(map_->getLabelLayer(), kSerializeOnlyUpdated,
                                  &gsm_update_msg.object.label_layer);

  gsm_update_msg.object.label = 0u;
  gsm_update_msg.old_labels.clear();
  gsm_update_msg.is_scene = true;
  geometry_msgs::Transform transform;
  transform.translation.x = 0.0;
  transform.translation.y = 0.0;
  transform.translation.z = 0.0;
  transform.rotation.w = 1.0;
  transform.rotation.x = 0.0;
  transform.rotation.y = 0.0;
  transform.rotation.z = 0.0;
  gsm_update_msg.object.transforms.clear();
  gsm_update_msg.object.transforms.push_back(transform);
  publishGsmUpdate(*scene_gsm_update_pub_, &gsm_update_msg);
}

void Controller::generateMesh(bool clear_mesh) {  // NOLINT
  voxblox::timing::Timer generate_mesh_timer("mesh/generate");
  {
    std::lock_guard<std::mutex> updateMeshLock(updated_mesh_mutex_);
    if (clear_mesh) {
      constexpr bool only_mesh_updated_blocks = false;
      constexpr bool clear_updated_flag = true;
      mesh_label_integrator_->generateMesh(only_mesh_updated_blocks,
                                           clear_updated_flag);
      if (enable_semantic_instance_segmentation_) {
        all_semantic_labels_.clear();
        mesh_semantic_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        for (auto sl : all_semantic_labels_) {
          LOG(ERROR) << classes[(unsigned)sl];
        }
        mesh_instance_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        mesh_merged_integrator_->generateMesh(only_mesh_updated_blocks,
                                              clear_updated_flag);
      }

    } else {
      constexpr bool only_mesh_updated_blocks = true;
      constexpr bool clear_updated_flag = true;
      mesh_label_integrator_->generateMesh(only_mesh_updated_blocks,
                                           clear_updated_flag);

      if (enable_semantic_instance_segmentation_) {
        mesh_semantic_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        mesh_instance_integrator_->generateMesh(only_mesh_updated_blocks,
                                                clear_updated_flag);
        mesh_merged_integrator_->generateMesh(only_mesh_updated_blocks,
                                              clear_updated_flag);
      }
    }
    generate_mesh_timer.Stop();

    updated_mesh_ = true;

    if (publish_scene_mesh_) {
      timing::Timer publish_mesh_timer("mesh/publish");
      voxblox_msgs::Mesh mesh_msg;
      generateVoxbloxMeshMsg(mesh_label_layer_, ColorMode::kColor, &mesh_msg);
      mesh_msg.header.frame_id = world_frame_;
      scene_mesh_pub_->publish(mesh_msg);
      publish_mesh_timer.Stop();
    }
  }

  if (!mesh_filename_.empty()) {
    timing::Timer output_mesh_timer("mesh/output");
    bool success = outputMeshLayerAsPly("label_" + mesh_filename_, false,
                                        *mesh_label_layer_);
    if (enable_semantic_instance_segmentation_) {
      success &= outputMeshLayerAsPly("semantic_" + mesh_filename_, false,
                                      *mesh_semantic_layer_);
      success &= outputMeshLayerAsPly("instance_" + mesh_filename_, false,
                                      *mesh_instance_layer_);
      success &= outputMeshLayerAsPly("merged_" + mesh_filename_, false,
                                      *mesh_merged_layer_);
    }
    output_mesh_timer.Stop();
    if (success) {
      ROS_INFO("Output file as PLY: %s", mesh_filename_.c_str());
    } else {
      ROS_INFO("Failed to output mesh as PLY: %s", mesh_filename_.c_str());
    }
  }

  ROS_INFO_STREAM("Mesh Timings: " << std::endl
                                   << voxblox::timing::Timing::Print());
}

void Controller::updateMeshEvent(const ros::TimerEvent& e) {
  {
    std::lock_guard<std::mutex> updateMeshLock(updated_mesh_mutex_);
    timing::Timer generate_mesh_timer("mesh/update");
    bool only_mesh_updated_blocks = true;
    if (need_full_remesh_) {
      only_mesh_updated_blocks = false;
      need_full_remesh_ = false;
    }
    bool clear_updated_flag = false;
    updated_mesh_ |= mesh_label_integrator_->generateMesh(
        only_mesh_updated_blocks, clear_updated_flag);

    if (enable_semantic_instance_segmentation_) {
      updated_mesh_ |= mesh_merged_integrator_->generateMesh(
          only_mesh_updated_blocks, clear_updated_flag);

      updated_mesh_ |= mesh_instance_integrator_->generateMesh(
          only_mesh_updated_blocks, clear_updated_flag);

      clear_updated_flag = true;
      updated_mesh_ |= mesh_semantic_integrator_->generateMesh(
          only_mesh_updated_blocks, clear_updated_flag);
    }
    generate_mesh_timer.Stop();

    if (publish_scene_mesh_) {
      timing::Timer publish_mesh_timer("mesh/publish");
      voxblox_msgs::Mesh mesh_msg;
      generateVoxbloxMeshMsg(mesh_label_layer_, ColorMode::kColor, &mesh_msg);
      mesh_msg.header.frame_id = world_frame_;
      scene_mesh_pub_->publish(mesh_msg);
      publish_mesh_timer.Stop();
    }
  }
}

bool Controller::noNewUpdatesReceived() const {
  if (received_first_message_ && no_update_timeout_ != 0.0) {
    return (ros::Time::now() - last_update_received_).toSec() >
           no_update_timeout_;
  }
  return false;
}

void Controller::publishGsmUpdate(const ros::Publisher& publisher,
                                  modelify_msgs::GsmUpdate* gsm_update) {
  publisher.publish(*gsm_update);
}

void Controller::getLabelsToPublish(const bool get_all,
                                    std::vector<Label>* labels) {
  CHECK_NOTNULL(labels);
  if (get_all) {
    *labels = map_->getLabelList();
    ROS_INFO("Publishing all segments");
  } else {
    *labels = segment_labels_to_publish_;
  }
}

void Controller::computeAlignedBoundingBox(
    const pcl::PointCloud<pcl::PointSurfel>::Ptr surfel_cloud,
    Eigen::Vector3f* bbox_translation, Eigen::Quaternionf* bbox_quaternion,
    Eigen::Vector3f* bbox_size) {
  CHECK(surfel_cloud);
  CHECK_NOTNULL(bbox_translation);
  CHECK_NOTNULL(bbox_quaternion);
  CHECK_NOTNULL(bbox_size);
#ifdef APPROXMVBB_AVAILABLE
  ApproxMVBB::Matrix3Dyn points(3, surfel_cloud->points.size());

  for (size_t i = 0u; i < surfel_cloud->points.size(); ++i) {
    points(0, i) = double(surfel_cloud->points.at(i).x);
    points(1, i) = double(surfel_cloud->points.at(i).y);
    points(2, i) = double(surfel_cloud->points.at(i).z);
  }

  constexpr double kEpsilon = 0.02;
  constexpr size_t kPointSamples = 200u;
  constexpr size_t kGridSize = 5u;
  constexpr size_t kMvbbDiamOptLoops = 0u;
  constexpr size_t kMvbbGridSearchOptLoops = 0u;
  ApproxMVBB::OOBB oobb =
      ApproxMVBB::approximateMVBB(points, kEpsilon, kPointSamples, kGridSize,
                                  kMvbbDiamOptLoops, kMvbbGridSearchOptLoops);

  ApproxMVBB::Matrix33 A_KI = oobb.m_q_KI.matrix().transpose();
  const int size = points.cols();
  for (unsigned int i = 0; i < size; ++i) {
    oobb.unite(A_KI * points.col(i));
  }

  constexpr double kExpansionPercentage = 0.05;
  oobb.expandToMinExtentRelative(kExpansionPercentage);

  ApproxMVBB::Vector3 min_in_I = oobb.m_q_KI * oobb.m_minPoint;
  ApproxMVBB::Vector3 max_in_I = oobb.m_q_KI * oobb.m_maxPoint;

  *bbox_quaternion = oobb.m_q_KI.cast<float>();
  *bbox_translation = ((min_in_I + max_in_I) / 2).cast<float>();
  *bbox_size = ((oobb.m_maxPoint - oobb.m_minPoint).cwiseAbs()).cast<float>();
#else
  ROS_WARN_STREAM(
      "Bounding box computation is not supported since ApproxMVBB is "
      "disabled.");
#endif
}

}  // namespace voxblox_gsm
}  // namespace voxblox
