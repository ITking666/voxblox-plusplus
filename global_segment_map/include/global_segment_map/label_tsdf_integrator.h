#ifndef GLOBAL_SEGMENT_MAP_LABEL_TSDF_INTEGRATOR_H_
#define GLOBAL_SEGMENT_MAP_LABEL_TSDF_INTEGRATOR_H_

#include <algorithm>
#include <list>
#include <map>
#include <utility>
#include <vector>

#include <glog/logging.h>
#include <voxblox/core/layer.h>
#include <voxblox/core/voxel.h>
#include <voxblox/integrator/integrator_utils.h>
#include <voxblox/integrator/tsdf_integrator.h>
#include <voxblox/utils/timing.h>
#include <boost/math/distributions/lognormal.hpp>

#include "global_segment_map/label_tsdf_map.h"
#include "global_segment_map/label_voxel.h"

namespace voxblox {

class Segment {
 public:
  voxblox::Pointcloud points_C_;
  voxblox::Transformation T_G_C_;
  voxblox::Colors colors_;
  voxblox::Labels labels_;
  voxblox::SemanticLabel semantic_label_;
  voxblox::SemanticLabel instance_;
};

class LabelTsdfIntegrator : public MergedTsdfIntegrator {
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  typedef LongIndexHashMapType<AlignedVector<size_t>>::type VoxelMap;
  typedef VoxelMap::value_type VoxelMapElement;
  typedef std::map<Label, int> LMap;
  typedef std::map<Label, int>::iterator LMapIt;
  typedef std::map<SemanticLabel, int> SLMap;
  typedef std::map<Label, LMap> LLMap;
  typedef std::map<Label, LMap>::iterator LLMapIt;
  typedef std::map<Label, SLMap> LSLMap;
  typedef std::set<Label> LSet;
  typedef std::set<Label>::iterator LSetIt;
  typedef std::map<Label, LSet> LLSet;
  typedef std::map<Label, LSet>::iterator LLSetIt;

  struct LabelTsdfConfig {
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW

    // TODO(grinvalm): maybe use a relative measure, not absolue voxel count.
    // Minimum number of label voxels count for label propagation.
    size_t min_label_voxel_count = 20;
    // Factor determining the label propagation truncation distance.
    float label_propagation_td_factor = 1.0;

    bool enable_pairwise_confidence_merging = true;
    float pairwise_confidence_ratio_threshold = 0.2f;
    int pairwise_confidence_count_threshold = 30;

    // Number of frames after which the updated
    // objects are published.
    int object_flushing_age_threshold = 3;

    // Distance-based log-normal distribution of label confidence weights.
    bool enable_confidence_weight_dropoff = false;
    float lognormal_weight_mean = 0.0f;
    float lognormal_weight_sigma = 1.8f;
    float lognormal_weight_offset = 0.7f;
  };

  LabelTsdfIntegrator(const Config& config,
                      const LabelTsdfConfig& label_tsdf_config,
                      Layer<TsdfVoxel>* tsdf_layer,
                      Layer<LabelVoxel>* label_layer, Label* highest_label)
      : MergedTsdfIntegrator(config, CHECK_NOTNULL(tsdf_layer)),
        label_tsdf_config_(label_tsdf_config),
        label_layer_(CHECK_NOTNULL(label_layer)),
        highest_label_(CHECK_NOTNULL(highest_label)),
        highest_instance_(0u) {
    CHECK(label_layer_);
  }

  inline void checkForSegmentLabelMergeCandidate(
      Label label, int label_points_count, int segment_points_count,
      std::unordered_set<Label>* merge_candidate_labels) {
    // All segment labels that overlap with more than a certain
    // percentage of the segment points are potential merge candidates.
    float label_segment_overlap_ratio =
        static_cast<float>(label_points_count) /
        static_cast<float>(segment_points_count);
    if (label_segment_overlap_ratio >
        label_tsdf_config_.pairwise_confidence_ratio_threshold) {
      merge_candidate_labels->insert(label);
    }
  }

  inline void increaseLabelCountForSegment(
      Segment* segment, Label label, int segment_points_count,
      std::map<Label, std::map<Segment*, size_t>>* candidates,
      std::unordered_set<Label>* merge_candidate_labels) {
    auto label_it = candidates->find(label);
    if (label_it != candidates->end()) {
      auto segment_it = label_it->second.find(segment);
      if (segment_it != label_it->second.end()) {
        ++segment_it->second;

        if (label_tsdf_config_.enable_pairwise_confidence_merging) {
          checkForSegmentLabelMergeCandidate(label, segment_it->second,
                                             segment_points_count,
                                             merge_candidate_labels);
        }
      } else {
        label_it->second.emplace(segment, 1u);
      }
    } else {
      std::map<Segment*, size_t> segment_points_count;
      segment_points_count.emplace(segment, 1u);
      candidates->emplace(label, segment_points_count);
    }
  }

  inline void increasePairwiseConfidenceCount(
      std::vector<Label> merge_candidates) {
    for (size_t i = 0u; i < merge_candidates.size(); ++i) {
      for (size_t j = i + 1; j < merge_candidates.size(); ++j) {
        Label new_label = merge_candidates[i];
        Label old_label = merge_candidates[j];

        if (new_label != old_label) {
          // Pairs consist of (new_label, old_label) where new_label <
          // old_label.
          if (new_label > old_label) {
            Label tmp = old_label;
            old_label = new_label;
            new_label = tmp;
          }
          // For every pair of labels from the merge candidates
          // set or increase their pairwise confidence.
          LLMapIt new_label_it = pairwise_confidence_.find(new_label);
          if (new_label_it != pairwise_confidence_.end()) {
            LMapIt old_label_it = new_label_it->second.find(old_label);
            if (old_label_it != new_label_it->second.end()) {
              ++old_label_it->second;
            } else {
              new_label_it->second.emplace(old_label, 1);
            }
          } else {
            LMap confidence_pair;
            confidence_pair.emplace(old_label, 1);
            pairwise_confidence_.emplace(new_label, confidence_pair);
          }
        }
      }
    }
  }

  Label getNextUnassignedLabel(const LabelVoxel& voxel,
                               const std::set<Label>& assigned_labels) {
    Label voxel_label = 0u;
    auto label_it = assigned_labels.find(voxel.label);
    if (label_it != assigned_labels.end()) {
      // The voxel label has been assigned already, so find
      // the next unassigned label with highest confidence for this voxel.
      LabelConfidence max_confidence = 0.0f;
      // TODO(grinvalm) in case of two or more labels
      // having the same confidence choose according to some logic,
      // not randomly.
      // Ideally one should avoid labels that belong to another segment
      // but not assigned in the current frame.
      Label preferred_label = 0u;
      for (const LabelCount& label_count : voxel.label_count) {
        label_it = assigned_labels.find(label_count.label);
        if (label_it == assigned_labels.end() &&
            (label_count.label_confidence >= max_confidence ||
             (label_count.label == preferred_label && preferred_label != 0u &&
              label_count.label_confidence == max_confidence))) {
          max_confidence = label_count.label_confidence;
          voxel_label = label_count.label;
        }
      }
    } else {
      // The voxel label hasn't been assigned yet, so it is valid.
      voxel_label = voxel.label;
    }
    return voxel_label;
  }

  void updateVoxelLabelAndConfidence(LabelVoxel* label_voxel,
                                     const Label& preferred_label = 0u) {
    Label max_label = 0u;
    LabelConfidence max_confidence = 0.0f;
    for (const LabelCount& label_count : label_voxel->label_count) {
      if (label_count.label_confidence > max_confidence ||
          (label_count.label == preferred_label && preferred_label != 0u &&
           label_count.label_confidence == max_confidence)) {
        max_confidence = label_count.label_confidence;
        max_label = label_count.label;
      }
    }
    label_voxel->label = max_label;
    label_voxel->label_confidence = max_confidence;
  }

  void addVoxelLabelConfidence(const Label& label,
                               const LabelConfidence& confidence,
                               LabelVoxel* label_voxel) {
    bool updated = false;
    for (LabelCount& label_count : label_voxel->label_count) {
      if (label_count.label == label) {
        // Label already observed in this voxel.
        label_count.label_confidence =
            label_count.label_confidence + confidence;
        updated = true;
        break;
      }
    }
    if (updated == false) {
      for (LabelCount& label_count : label_voxel->label_count) {
        if (label_count.label == 0u) {
          // This is the first allocated but unused index in the map
          // in which the new entry should be added.
          label_count.label = label;
          label_count.label_confidence = confidence;
          updated = true;
          break;
        }
      }
    }
    if (updated == false) {
      // LOG(FATAL) << "Out-of-memory for storing labels and confidences for
      // this " "voxel. Please increse size of array.";
    }
  }

  inline void computeSegmentLabelCandidates(
      Segment* segment, std::map<Label, std::map<Segment*, size_t>>* candidates,
      std::map<Segment*, std::vector<Label>>* segment_merge_candidates,
      const std::set<Label>& assigned_labels = std::set<Label>()) {
    DCHECK(segment != nullptr);
    DCHECK(candidates != nullptr);
    // Flag to check whether there exists at least one label candidate.
    bool candidate_label_exists = false;
    const int segment_points_count = segment->points_C_.size();
    std::unordered_set<Label> merge_candidate_labels;

    for (const Point& point_C : segment->points_C_) {
      const Point point_G = segment->T_G_C_ * point_C;

      // Get the corresponding voxel by 3D position in world frame.
      Layer<LabelVoxel>::BlockType::ConstPtr label_block_ptr =
          label_layer_->getBlockPtrByCoordinates(point_G);
      // Get the corresponding voxel by 3D position in world frame.
      Layer<TsdfVoxel>::BlockType::ConstPtr tsdf_block_ptr =
          layer_->getBlockPtrByCoordinates(point_G);

      if (label_block_ptr != nullptr) {
        const LabelVoxel& label_voxel =
            label_block_ptr->getVoxelByCoordinates(point_G);
        const TsdfVoxel& tsdf_voxel =
            tsdf_block_ptr->getVoxelByCoordinates(point_G);
        Label label = 0u;
        label = getNextUnassignedLabel(label_voxel, assigned_labels);
        if (label != 0u &&
            std::abs(tsdf_voxel.distance) <
                label_tsdf_config_.label_propagation_td_factor * voxel_size_) {
          // Do not consider allocated but unobserved voxels
          // which have label == 0.
          candidate_label_exists = true;
          increaseLabelCountForSegment(segment, label, segment_points_count,
                                       candidates, &merge_candidate_labels);
        }
      }
    }

    if (label_tsdf_config_.enable_pairwise_confidence_merging) {
      std::vector<Label> merge_candidates;
      std::copy(merge_candidate_labels.begin(), merge_candidate_labels.end(),
                std::back_inserter(merge_candidates));
      (*segment_merge_candidates)[segment] = merge_candidates;
    }

    // Previously unobserved segment gets an unseen label.
    if (!candidate_label_exists) {
      Label fresh_label = getFreshLabel();
      std::map<Segment*, size_t> map;
      map.insert(
          std::pair<Segment*, size_t>(segment, segment->points_C_.size()));
      candidates->insert(
          std::pair<Label, std::map<Segment*, size_t>>(fresh_label, map));
    }
  }

  // Fetch the next segment label pair which has overall
  // the highest voxel count.
  inline bool getNextSegmentLabelPair(
      const std::set<Segment*>& labelled_segments,
      std::set<Label>* assigned_labels,
      std::map<voxblox::Label, std::map<voxblox::Segment*, size_t>>* candidates,
      std::map<Segment*, std::vector<Label>>* segment_merge_candidates,
      std::pair<Segment*, Label>* segment_label_pair) {
    Label max_label;
    size_t max_count = 0u;
    Segment* max_segment;
    std::map<voxblox::Segment*, size_t> segments_to_recompute;

    for (auto label_it = candidates->begin(); label_it != candidates->end();
         ++label_it) {
      for (auto segment_it = label_it->second.begin();
           segment_it != label_it->second.end(); segment_it++) {
        if (segment_it->second > max_count &&
            segment_it->second > label_tsdf_config_.min_label_voxel_count &&
            labelled_segments.find(segment_it->first) ==
                labelled_segments.end()) {
          max_count = segment_it->second;
          max_segment = segment_it->first;
          max_label = label_it->first;
          segments_to_recompute = label_it->second;
        }
      }
    }
    if (max_count == 0u) {
      return false;
    }

    segment_label_pair->first = max_segment;
    segment_label_pair->second = max_label;
    assigned_labels->emplace(max_label);

    // For all segments that need to have their label
    // count recomputed, first clean their relative entries and recompute.
    for (auto segment : segments_to_recompute) {
      if (segment.first != max_segment) {
        for (auto label_it = candidates->begin(); label_it != candidates->end();
             ++label_it) {
          if (label_it->first != max_label) {
            label_it->second.erase(segment.first);
          }
        }
        computeSegmentLabelCandidates(segment.first, candidates,
                                      segment_merge_candidates,
                                      *assigned_labels);
      }
    }
    return true;
  }

  inline void decideLabelPointClouds(
      std::vector<voxblox::Segment*>* segments_to_integrate,
      std::map<voxblox::Label, std::map<voxblox::Segment*, size_t>>* candidates,
      std::map<Segment*, std::vector<Label>>* segment_merge_candidates) {
    std::set<Label> assigned_labels;
    std::set<Segment*> labelled_segments;
    std::pair<Segment*, Label> pair;
    std::set<SemanticLabel> assigned_instances;

    while (getNextSegmentLabelPair(labelled_segments, &assigned_labels,
                                   candidates, segment_merge_candidates,
                                   &pair)) {
      Segment* segment = pair.first;
      Label& label = pair.second;
      for (size_t i = 0u; i < segment->points_C_.size(); ++i) {
        segment->labels_.push_back(label);
      }

      labelled_segments.insert(pair.first);
      candidates->erase(pair.second);
    }

    for (auto merge_candidates : *segment_merge_candidates) {
      increasePairwiseConfidenceCount(merge_candidates.second);
    }

    // For every segment that didn't get a label because
    // its label counts were too few, assign it an unseen label.
    for (auto segment_it = segments_to_integrate->begin();
         segment_it != segments_to_integrate->end(); ++segment_it) {
      if (labelled_segments.find(*segment_it) == labelled_segments.end()) {
        Label fresh = getFreshLabel();
        for (size_t i = 0u; i < (*segment_it)->points_C_.size(); ++i) {
          (*segment_it)->labels_.push_back(fresh);
        }
        labelled_segments.insert(*segment_it);
      }
    }

    // Instance stuff.

    for (auto segment_it = labelled_segments.begin();
         segment_it != labelled_segments.end(); ++segment_it) {
      Label label = (*segment_it)->labels_[0];
      if ((*segment_it)->points_C_.size() > 0) {
        increaseLabelFramesCount(label);
      }

      // Loop through all the segments.
      if ((*segment_it)->instance_ != 0u) {
        // It's a segment with a current frame instance.
        auto global_instance_it =
            current_to_global_instance_map_.find((*segment_it)->instance_);
        if (global_instance_it != current_to_global_instance_map_.end()) {
          // If current frame instance maps to a global instance, use it.
          increaseLabelInstanceCount(label, global_instance_it->second);
        } else {
          // Current frame instance doesn't map to any global instance.
          // Get the global instance with max count.
          SemanticLabel instance_label =
              getLabelInstance(label, assigned_instances);

          if (instance_label != 0u) {
            current_to_global_instance_map_.emplace((*segment_it)->instance_,
                                                    instance_label);
            increaseLabelInstanceCount(label, instance_label);
            assigned_instances.emplace(instance_label);
          } else {
            // Create new global instance.
            SemanticLabel fresh_instance = getFreshInstance();
            current_to_global_instance_map_.emplace((*segment_it)->instance_,
                                                    fresh_instance);
            increaseLabelInstanceCount(label, fresh_instance);
          }
        }
        increaseLabelClassCount(label, (*segment_it)->semantic_label_);
      } else {
        // It's a segment with no instance prediction in the current frame.
        // Get the global instance it maps to, as set it as assigned.
        SemanticLabel instance_label = getLabelInstance(label);
        // TODO(grinvalm) : also pass assigned instances here?
        if (instance_label != 0u) {
          assigned_instances.emplace(instance_label);
        }
        // if ((*segment_it)->points_C_.size() > 2500) {
        //   decreaseLabelInstanceCount(label, instance_label);
        // }
      }
    }
  }

  SemanticLabel getLabelInstance(const Label& label) {
    std::set<SemanticLabel> assigned_instances;
    return getLabelInstance(label, assigned_instances);
  }

  SemanticLabel getLabelInstance(const Label& label,
                                 std::set<SemanticLabel>& assigned_instances) {
    SemanticLabel instance_label = 0u;
    int max_count = 0;
    auto label_it = label_instance_count_.find(label);
    if (label_it != label_instance_count_.end()) {
      for (auto const& instance_count : label_it->second) {
        if (instance_count.second > max_count && instance_count.first != 0u &&
            assigned_instances.find(instance_count.first) ==
                assigned_instances.end()) {
          int frames_count = 0;
          auto label_count_it = label_frames_count_.find(label);
          if (label_count_it != label_frames_count_.end()) {
            frames_count = label_count_it->second;
          }
          if (instance_count.second >
              0.0 * (float)(frames_count - instance_count.second)) {
            instance_label = instance_count.first;
            max_count = instance_count.second;
          }
        }
      }
    } else {
      // LOG(ERROR) << "No semantic class for label?";
    }
    return instance_label;
  }

  void increaseLabelFramesCount(const Label& label) {
    auto label_count_it = label_frames_count_.find(label);
    if (label_count_it != label_frames_count_.end()) {
      ++label_count_it->second;
    } else {
      label_frames_count_.insert(std::make_pair(label, 1));
    }
  }

  // Increase or decrease the voxel count for a label.
  inline void changeLabelCount(const Label label, int count) {
    auto label_count_it = labels_count_map_.find(label);
    if (label_count_it != labels_count_map_.end()) {
      label_count_it->second = label_count_it->second + count;
      if (label_count_it->second <= 0) {
        labels_count_map_.erase(label_count_it);
      }
    } else {
      if (label != 0u) {
        DCHECK(count > 0);
        labels_count_map_.insert(std::make_pair(label, count));
      }
    }
  }

  // Will return a pointer to a voxel located at global_voxel_idx in the label
  // layer. Thread safe.
  // Takes in the last_block_idx and last_block to prevent unneeded map
  // lookups. If the block this voxel would be in has not been allocated, a
  // block in temp_label_block_map_ is created/accessed and a voxel from this
  // map is returned instead. Unlike the layer, accessing
  // temp_label_block_map_ is controlled via a mutex allowing it to grow
  // during integration. These temporary blocks can be merged into the layer
  // later by calling updateLayerWithStoredBlocks()
  LabelVoxel* allocateStorageAndGetLabelVoxelPtr(
      const GlobalIndex& global_voxel_idx, Block<LabelVoxel>::Ptr* last_block,
      BlockIndex* last_block_idx) {
    DCHECK(last_block != nullptr);
    DCHECK(last_block_idx != nullptr);

    const BlockIndex block_idx = getBlockIndexFromGlobalVoxelIndex(
        global_voxel_idx, voxels_per_side_inv_);

    if ((block_idx != *last_block_idx) || (*last_block == nullptr)) {
      *last_block = label_layer_->getBlockPtrByIndex(block_idx);
      *last_block_idx = block_idx;
    }

    // If no block at this location currently exists, we allocate a temporary
    // voxel that will be merged into the map later
    if (*last_block == nullptr) {
      // To allow temp_label_block_map_ to grow we can only let
      // one thread in at once
      std::lock_guard<std::mutex> lock(temp_label_block_mutex_);

      typename Layer<LabelVoxel>::BlockHashMap::iterator it =
          temp_label_block_map_.find(block_idx);
      if (it != temp_label_block_map_.end()) {
        *last_block = it->second;
      } else {
        auto insert_status = temp_label_block_map_.emplace(
            block_idx,
            std::make_shared<Block<LabelVoxel>>(
                voxels_per_side_, voxel_size_,
                getOriginPointFromGridIndex(block_idx, block_size_)));

        DCHECK(insert_status.second)
            << "Block already exists when allocating at "
            << block_idx.transpose();

        *last_block = insert_status.first->second;
      }
    }

    (*last_block)->updated() = true;

    const VoxelIndex local_voxel_idx =
        getLocalFromGlobalVoxelIndex(global_voxel_idx, voxels_per_side_);

    return &((*last_block)->getVoxelByVoxelIndex(local_voxel_idx));
  }

  // NOT thread safe
  void updateLabelLayerWithStoredBlocks() {
    BlockIndex last_block_idx;
    Block<LabelVoxel>::Ptr block = nullptr;

    for (const std::pair<const BlockIndex, Block<LabelVoxel>::Ptr>&
             temp_label_block_pair : temp_label_block_map_) {
      label_layer_->insertBlock(temp_label_block_pair);
    }

    temp_label_block_map_.clear();
  }

  void increaseLabelInstanceCount(const Label& label,
                                  const SemanticLabel& instance_label) {
    auto label_it = label_instance_count_.find(label);
    if (label_it != label_instance_count_.end()) {
      auto instance_it = label_it->second.find(instance_label);
      if (instance_it != label_it->second.end()) {
        ++instance_it->second;
      } else {
        label_it->second.emplace(instance_label, 1);
      }
    } else {
      SLMap instance_count;
      instance_count.emplace(instance_label, 1);
      label_instance_count_.emplace(label, instance_count);
    }
    // for (auto label_it : label_instance_count_) {
    //   LOG(ERROR) << "Loop labels";
    //   for (auto instance_it : label_it.second) {
    //     LOG(ERROR) << "Count " << label_it.first << " for instance "
    //                << unsigned(instance_it.first) << " is "
    //                << instance_it.second;
    //   }
    // }
  }

  void decreaseLabelInstanceCount(const Label& label,
                                  const SemanticLabel& instance_label) {
    auto label_it = label_instance_count_.find(label);
    if (label_it != label_instance_count_.end()) {
      auto instance_it = label_it->second.find(instance_label);
      if (instance_it != label_it->second.end()) {
        --instance_it->second;
      } else {
        // label_it->second.emplace(instance_label, 1);
      }
    } else {
      // SLMap instance_count;
      // instance_count.emplace(instance_label, 1);
      // label_instance_count_.emplace(label, instance_count);
    }
  }

  void increaseLabelClassCount(const Label& label,
                               const SemanticLabel& semantic_label) {
    auto label_it = label_class_count_.find(label);
    if (label_it != label_class_count_.end()) {
      auto class_it = label_it->second.find(semantic_label);
      if (class_it != label_it->second.end()) {
        ++class_it->second;
      } else {
        label_it->second.emplace(semantic_label, 1);
      }
    } else {
      SLMap class_points_count;
      class_points_count.emplace(semantic_label, 1);
      label_class_count_.emplace(label, class_points_count);
    }
  }

  // Updates label_voxel. Thread safe.
  inline void updateLabelVoxel(const Point& point_G, const Label& label,
                               LabelVoxel* label_voxel,
                               const LabelConfidence& confidence = 1.0f) {
    // Lookup the mutex that is responsible for this voxel and lock it.
    std::lock_guard<std::mutex> lock(mutexes_.get(
        getGridIndexFromPoint<GlobalIndex>(point_G, voxel_size_inv_)));

    CHECK_NOTNULL(label_voxel);

    // label_voxel->semantic_label = semantic_label;
    Label previous_label = label_voxel->label;
    addVoxelLabelConfidence(label, confidence, label_voxel);
    updateVoxelLabelAndConfidence(label_voxel, label);
    Label new_label = label_voxel->label;

    // This old semantic stuff per voxel was not thread safe.
    // Now all is good.
    // increaseLabelClassCount(new_label, semantic_label);

    if (new_label != previous_label) {
      // Both of the segments corresponding to the two labels are
      // updated, one gains a voxel, one loses a voxel.
      std::lock_guard<std::mutex> lock(updated_labels_mutex_);

      updated_labels_.insert(new_label);
      changeLabelCount(new_label, 1);

      if (previous_label != 0u) {
        updated_labels_.insert(previous_label);
        changeLabelCount(previous_label, -1);
      }

      if (*highest_label_ < new_label) {
        *highest_label_ = new_label;
      }
    }
  }

  void integratePointCloud(const Transformation& T_G_C,
                           const Pointcloud& points_C, const Colors& colors,
                           const Labels& labels,

                           const bool freespace_points) {
    CHECK_EQ(points_C.size(), colors.size());
    CHECK_EQ(points_C.size(), labels.size());
    // CHECK_EQ(points_C.size(), semantic_labels.size());
    CHECK_GE(points_C.size(), 0u);

    timing::Timer integrate_timer("integrate");

    // Pre-compute a list of unique voxels to end on.
    // Create a hashmap: VOXEL INDEX -> index in original cloud.
    VoxelMap voxel_map;
    // This is a hash map (same as above) to all the indices that need to be
    // cleared.
    VoxelMap clear_map;

    ThreadSafeIndex index_getter(points_C.size());

    bundleRays(T_G_C, points_C, freespace_points, &index_getter, &voxel_map,
               &clear_map);

    integrateRays(T_G_C, points_C, colors, labels, config_.enable_anti_grazing,
                  false, voxel_map, clear_map);

    timing::Timer clear_timer("integrate/clear");

    integrateRays(T_G_C, points_C, colors, labels, config_.enable_anti_grazing,
                  true, voxel_map, clear_map);

    clear_timer.Stop();

    integrate_timer.Stop();
  }

  void integrateVoxel(const Transformation& T_G_C, const Pointcloud& points_C,
                      const Colors& colors, const Labels& labels,
                      const bool enable_anti_grazing, const bool clearing_ray,
                      const VoxelMapElement& global_voxel_idx_to_point_indices,
                      const VoxelMap& voxel_map) {
    if (global_voxel_idx_to_point_indices.second.empty()) {
      return;
    }

    const Point& origin = T_G_C.getPosition();
    Color merged_color;
    Point merged_point_C = Point::Zero();
    FloatingPoint merged_weight = 0.0f;
    Label merged_label;
    LabelConfidence merged_label_confidence;

    for (const size_t pt_idx : global_voxel_idx_to_point_indices.second) {
      const Point& point_C = points_C[pt_idx];
      const Color& color = colors[pt_idx];
      const Label& label = labels[pt_idx];

      const float point_weight = getVoxelWeight(point_C);
      merged_point_C =
          (merged_point_C * merged_weight + point_C * point_weight) /
          (merged_weight + point_weight);
      merged_color = Color::blendTwoColors(merged_color, merged_weight, color,
                                           point_weight);
      merged_weight += point_weight;
      // Assuming all the points of a segment pointcloud
      // are assigned the same label.
      merged_label = label;
      if (label_tsdf_config_.enable_confidence_weight_dropoff) {
        const FloatingPoint ray_distance = point_C.norm();
        merged_label_confidence = computeConfidenceWeight(ray_distance);
      } else {
        merged_label_confidence = 1.0f;
      }

      // only take first point when clearing
      if (clearing_ray) {
        break;
      }
    }

    const Point merged_point_G = T_G_C * merged_point_C;

    RayCaster ray_caster(origin, merged_point_G, clearing_ray,
                         config_.voxel_carving_enabled,
                         config_.max_ray_length_m, voxel_size_inv_,
                         config_.default_truncation_distance);

    GlobalIndex global_voxel_idx;
    while (ray_caster.nextRayIndex(&global_voxel_idx)) {
      if (enable_anti_grazing) {
        // Check if this one is already the block hash map for this
        // insertion. Skip this to avoid grazing.
        if ((clearing_ray ||
             global_voxel_idx != global_voxel_idx_to_point_indices.first) &&
            voxel_map.find(global_voxel_idx) != voxel_map.end()) {
          continue;
        }
      }
      BlockIndex block_idx;

      Block<TsdfVoxel>::Ptr tsdf_block = nullptr;
      TsdfVoxel* tsdf_voxel = allocateStorageAndGetVoxelPtr(
          global_voxel_idx, &tsdf_block, &block_idx);

      updateTsdfVoxel(origin, merged_point_G, global_voxel_idx, merged_color,
                      merged_weight, tsdf_voxel);

      // If voxel carving is enabled, then only allocate the label voxels
      // within twice the truncation distance from the surface.
      if (!config_.voxel_carving_enabled ||
          std::abs(tsdf_voxel->distance) <
              10 * config_.default_truncation_distance) {
        Block<LabelVoxel>::Ptr label_block = nullptr;
        LabelVoxel* label_voxel = allocateStorageAndGetLabelVoxelPtr(
            global_voxel_idx, &label_block, &block_idx);
        updateLabelVoxel(merged_point_G, merged_label, label_voxel,
                         merged_label_confidence);
      }
    }
  }

  void integrateVoxels(const Transformation& T_G_C, const Pointcloud& points_C,
                       const Colors& colors, const Labels& labels,
                       const bool enable_anti_grazing, const bool clearing_ray,
                       const VoxelMap& voxel_map, const VoxelMap& clear_map,
                       const size_t thread_idx) {
    VoxelMap::const_iterator it;
    size_t map_size;
    if (clearing_ray) {
      it = clear_map.begin();
      map_size = clear_map.size();
    } else {
      it = voxel_map.begin();
      map_size = voxel_map.size();
    }
    for (size_t i = 0; i < map_size; ++i) {
      if (((i + thread_idx + 1) % config_.integrator_threads) == 0) {
        integrateVoxel(T_G_C, points_C, colors, labels, enable_anti_grazing,
                       clearing_ray, *it, voxel_map);
      }
      ++it;
    }
  }

  void integrateRays(const Transformation& T_G_C, const Pointcloud& points_C,
                     const Colors& colors, const Labels& labels,
                     const bool enable_anti_grazing, const bool clearing_ray,
                     const VoxelMap& voxel_map, const VoxelMap& clear_map) {
    const Point& origin = T_G_C.getPosition();

    // if only 1 thread just do function call, otherwise spawn threads
    if (config_.integrator_threads == 1) {
      constexpr size_t thread_idx = 0;
      integrateVoxels(T_G_C, points_C, colors, labels, enable_anti_grazing,
                      clearing_ray, voxel_map, clear_map, thread_idx);
    } else {
      std::list<std::thread> integration_threads;
      for (size_t i = 0; i < config_.integrator_threads; ++i) {
        integration_threads.emplace_back(&LabelTsdfIntegrator::integrateVoxels,
                                         this, T_G_C, points_C, colors, labels,
                                         enable_anti_grazing, clearing_ray,
                                         voxel_map, clear_map, i);
      }

      for (std::thread& thread : integration_threads) {
        thread.join();
      }
    }

    timing::Timer insertion_timer("inserting_missed_blocks");
    updateLayerWithStoredBlocks();
    updateLabelLayerWithStoredBlocks();

    insertion_timer.Stop();
  }

  FloatingPoint computeConfidenceWeight(FloatingPoint distance) {
    boost::math::lognormal_distribution<> distribution(
        label_tsdf_config_.lognormal_weight_mean,
        label_tsdf_config_.lognormal_weight_sigma);

    return pdf(
        distribution,
        std::max(0.0f, distance - label_tsdf_config_.lognormal_weight_offset));
  }

  // Not thread safe.
  void swapLabels(Label old_label, Label new_label) {
    BlockIndexList all_label_blocks;
    label_layer_->getAllAllocatedBlocks(&all_label_blocks);

    for (const BlockIndex& block_index : all_label_blocks) {
      Block<LabelVoxel>::Ptr block =
          label_layer_->getBlockPtrByIndex(block_index);
      size_t vps = block->voxels_per_side();
      for (int i = 0; i < vps * vps * vps; i++) {
        LabelVoxel& voxel = block->getVoxelByLinearIndex(i);
        Label previous_label = voxel.label;

        LabelConfidence old_label_confidence = 0.0f;
        for (LabelCount& label_count : voxel.label_count) {
          if (label_count.label == old_label) {
            // Store confidence for old_label and remove that entry.
            old_label_confidence = label_count.label_confidence;
            label_count.label = 0u;
            label_count.label_confidence = 0.0f;
          }
        }
        if (old_label_confidence > 0.0f) {
          // Add old_label confidence, if any, to new_label confidence.
          addVoxelLabelConfidence(new_label, old_label_confidence, &voxel);
        }
        // TODO(grinvalm) calling update with different preferred labels
        // can result in different assigned labels to the voxel, and
        // can trigger an update of a segment.
        updateVoxelLabelAndConfidence(&voxel, new_label);
        Label updated_label = voxel.label;

        if (updated_label != previous_label) {
          // The new updated_label gains a voxel.
          updated_labels_.insert(updated_label);
          changeLabelCount(updated_label, 1);

          changeLabelCount(previous_label, -1);
          block->updated() = true;
        }
      }
    }
  }

  void clearCurrentFrameInstanceLabels() {
    current_to_global_instance_map_.clear();
  }

  void resetCurrentFrameUpdatedLabelsAge() {
    for (Label label : updated_labels_) {
      // Set timestamp or integer age of segment.
      // Here is the place to do it so it's the same timestamp
      // for all segments in a frame.
      labels_to_publish_[label] = 0;
    }
    updated_labels_.clear();
  }

  void getLabelsToPublish(
      std::vector<voxblox::Label>* segment_labels_to_publish) {
    CHECK_NOTNULL(segment_labels_to_publish);
    resetCurrentFrameUpdatedLabelsAge();
    clearCurrentFrameInstanceLabels();

    for (LMapIt label_age_pair_it = labels_to_publish_.begin();
         label_age_pair_it != labels_to_publish_.end();
         /* no increment */) {
      // Increase age of a label to publish;
      ++(label_age_pair_it)->second;
      if (label_age_pair_it->second >
          label_tsdf_config_.object_flushing_age_threshold) {
        segment_labels_to_publish->push_back(label_age_pair_it->first);
        labels_to_publish_.erase(label_age_pair_it++);
      } else {
        ++label_age_pair_it;
      }
    }
  }

  LMap* getLabelsAgeMapPtr() {
    return &labels_to_publish_;
  }

  LSLMap* getLabelClassCountPtr() { return &label_class_count_; }

  LSLMap* getLabelInstanceCountPtr() { return &label_instance_count_; }

  LMap* getLabelsFrameCountPtr() { return &label_frames_count_; }

  void addPairwiseConfidenceCount(LLMapIt label_map_it, Label label,
                                  int count) {
    LMapIt label_count_it = label_map_it->second.find(label);
    if (label_count_it != label_map_it->second.end()) {
      // label_map already contains a pairwise confidence count
      // to label, increase existing value by count.
      label_map_it->second[label] = label_count_it->second + count;
    } else {
      label_map_it->second.emplace(label, count);
    }
  }

  void adjustPairwiseConfidenceAfterMerging(const Label& new_label,
                                            const Label& old_label) {
    // Add all the pairwise confidence counts of the old_label to new_label.
    // First the counts (old_label -> some_label),
    // where old_label < some_label.
    LLMapIt old_label_pc_it = pairwise_confidence_.find(old_label);
    if (old_label_pc_it != pairwise_confidence_.end()) {
      LLMapIt new_label_pc_it = pairwise_confidence_.find(new_label);
      if (new_label_pc_it != pairwise_confidence_.end()) {
        for (LMapIt old_label_pc_count_it = old_label_pc_it->second.begin();
             old_label_pc_count_it != old_label_pc_it->second.end();
             ++old_label_pc_count_it) {
          addPairwiseConfidenceCount(new_label_pc_it,
                                     old_label_pc_count_it->first,
                                     old_label_pc_count_it->second);
        }
      } else {
        LMap old_label_map(old_label_pc_it->second.begin(),
                           old_label_pc_it->second.end());
        pairwise_confidence_.emplace(new_label, old_label_map);
      }
      pairwise_confidence_.erase(old_label_pc_it);
    }

    // Next add the counts (some_label -> old_label),
    // where some_label < old_label.
    for (LLMapIt confidence_map_it = pairwise_confidence_.begin();
         confidence_map_it != pairwise_confidence_.end();
         /* no increment */) {
      for (LMapIt confidence_pair_it = confidence_map_it->second.begin();
           confidence_pair_it != confidence_map_it->second.end();
           /* no increment */) {
        if (confidence_pair_it->first == old_label) {
          if (confidence_map_it->first < new_label) {
            addPairwiseConfidenceCount(confidence_map_it, new_label,
                                       confidence_pair_it->second);
          } else {
            LLMapIt new_label_pc_it = pairwise_confidence_.find(new_label);
            if (new_label_pc_it != pairwise_confidence_.end()) {
              addPairwiseConfidenceCount(new_label_pc_it,
                                         confidence_map_it->first,
                                         confidence_pair_it->second);
            } else {
              LMap old_label_map;
              old_label_map.emplace(confidence_map_it->first,
                                    confidence_pair_it->second);
              pairwise_confidence_.emplace(new_label, old_label_map);
            }
          }
          confidence_pair_it =
              confidence_map_it->second.erase(confidence_pair_it);
        } else {
          ++confidence_pair_it;
        }
      }
      if (confidence_map_it->second.empty()) {
        confidence_map_it = pairwise_confidence_.erase(confidence_map_it);
      } else {
        ++confidence_map_it;
      }
    }
  }

  bool getNextMerge(Label* new_label, Label* old_label) {
    CHECK_NOTNULL(new_label);
    CHECK_NOTNULL(old_label);
    for (LLMapIt confidence_map_it = pairwise_confidence_.begin();
         confidence_map_it != pairwise_confidence_.end(); ++confidence_map_it) {
      for (LMapIt confidence_pair_it = confidence_map_it->second.begin();
           confidence_pair_it != confidence_map_it->second.end();
           ++confidence_pair_it) {
        if (confidence_pair_it->second >
            label_tsdf_config_.pairwise_confidence_count_threshold) {
          // If the pairwise confidence is above a threshold return
          // the two labels to merge and remove the pair
          // from the pairwise confidence counts.
          *new_label = confidence_map_it->first;
          *old_label = confidence_pair_it->first;
          confidence_pair_it =
              confidence_map_it->second.erase(confidence_pair_it);
          return true;
        }
      }
    }
    return false;
  }

  // Not thread safe.
  void mergeLabels(LLSet* merges_to_publish) {
    if (label_tsdf_config_.enable_pairwise_confidence_merging) {
      Label new_label;
      Label old_label;
      while (getNextMerge(&new_label, &old_label)) {
        LOG(ERROR) << "Merging labels " << new_label << " and " << old_label;
        swapLabels(old_label, new_label);

        // Delete any staged segment publishing for overridden label.
        LMapIt label_age_pair_it = labels_to_publish_.find(old_label);
        if (label_age_pair_it != labels_to_publish_.end()) {
          labels_to_publish_.erase(old_label);
        }
        updated_labels_.erase(old_label);

        // Store the happened merge.
        LLSetIt label_it = merges_to_publish->find(new_label);
        if (label_it != merges_to_publish->end()) {
          // If the new_label already incorporated other labels
          // just add the just incorporated old_label to this list.
          label_it->second.emplace(old_label);
        } else {
          // If the new_label hasn't incorporated any other labels yet
          // create a new list and only add the just incorporated
          // old_label.
          std::set<Label> incorporated_labels;
          incorporated_labels.emplace(old_label);
          merges_to_publish->emplace(new_label, incorporated_labels);
        }
        adjustPairwiseConfidenceAfterMerging(new_label, old_label);
      }
    }
  }

  Label getFreshLabel() {
    CHECK_LT(*highest_label_, std::numeric_limits<unsigned int>::max());
    return ++(*highest_label_);
  }

  Label getFreshInstance() {
    CHECK_LT(highest_instance_, std::numeric_limits<unsigned int>::max());
    return ++highest_instance_;
  }

  // Get the list of all labels
  // for which the voxel count is greater than 0.
  std::vector<Label> getLabelsList() {
    std::vector<Label> labels;
    for (std::pair<Label, int> label_count_pair : labels_count_map_) {
      if (label_count_pair.second > 0) {
        labels.push_back(label_count_pair.first);
      }
    }
    return labels;
  }

 protected:
  LabelTsdfConfig label_tsdf_config_;
  Layer<LabelVoxel>* label_layer_;

  // Temporary block storage, used to hold blocks that need to be created
  // while integrating a new pointcloud.
  std::mutex temp_label_block_mutex_;
  Layer<LabelVoxel>::BlockHashMap temp_label_block_map_;

  Label* highest_label_;
  LMap labels_count_map_;

  // Pairwise confidence merging.
  LLMap pairwise_confidence_;

  // Per frame voxel count of semantic label.
  LSLMap label_class_count_;

  SemanticLabel highest_instance_;
  std::map<SemanticLabel, SemanticLabel> current_to_global_instance_map_;
  LSLMap label_instance_count_;
  LMap label_frames_count_;

  // We need to prevent simultaneous access to the voxels in the map. We
  // could
  // put a single mutex on the map or on the blocks, but as voxel updating
  // is
  // the most expensive operation in integration and most voxels are close
  // together, both strategies would bottleneck the system. We could make a
  // mutex per voxel, but this is too ram heavy as one mutex = 40 bytes.
  // Because of this we create an array that is indexed by the first n bits
  // of
  // the voxels hash. Assuming a uniform hash distribution, this means the
  // chance of two threads needing the same lock for unrelated voxels is
  // (num_threads / (2^n)). For 8 threads and 12 bits this gives 0.2%.
  ApproxHashArray<12, std::mutex, GlobalIndex, LongIndexHash> mutexes_;

  std::mutex updated_labels_mutex_;
  std::set<Label> updated_labels_;

  LMap labels_to_publish_;
};  // namespace voxblox

}  // namespace voxblox

#endif  // GLOBAL_SEGMENT_MAP_LABEL_TSDF_INTEGRATOR_H_
