#include "camera_motion_tracker.h"

#include <algorithm>

void CameraMotionTracker::BeginFrame(size_t width, size_t height,
                                     float threshold) {
    width_ = width;
    height_ = height;
    threshold_ = threshold;
    active_cells_ = 0;
    min_x_ = width;
    min_y_ = height;
    max_x_ = 0;
    max_y_ = 0;
    total_weight_ = 0.0f;
    weighted_x_ = 0.0f;
    weighted_y_ = 0.0f;
}

void CameraMotionTracker::AddCell(size_t x, size_t y, float difference) {
    if (width_ == 0 || height_ == 0 || x >= width_ || y >= height_ ||
        difference < threshold_) {
        return;
    }
    ++active_cells_;
    min_x_ = std::min(min_x_, x);
    min_y_ = std::min(min_y_, y);
    max_x_ = std::max(max_x_, x);
    max_y_ = std::max(max_y_, y);
    total_weight_ += difference;
    weighted_x_ += (static_cast<float>(x) + 0.5f) * difference;
    weighted_y_ += (static_cast<float>(y) + 0.5f) * difference;
}

CameraMotionSpatialMetrics CameraMotionTracker::FinishFrame() const {
    CameraMotionSpatialMetrics metrics;
    if (active_cells_ == 0 || width_ == 0 || height_ == 0 ||
        total_weight_ <= 0.0f) {
        return metrics;
    }

    metrics.valid = true;
    metrics.active_cells = active_cells_;
    metrics.centroid_x = std::clamp(
        weighted_x_ / total_weight_ / static_cast<float>(width_), 0.0f, 1.0f);
    metrics.centroid_y = std::clamp(
        weighted_y_ / total_weight_ / static_cast<float>(height_), 0.0f, 1.0f);
    metrics.bbox_left = std::clamp(
        static_cast<float>(min_x_) / static_cast<float>(width_), 0.0f, 1.0f);
    metrics.bbox_top = std::clamp(
        static_cast<float>(min_y_) / static_cast<float>(height_), 0.0f, 1.0f);
    metrics.bbox_right = std::clamp(
        static_cast<float>(max_x_ + 1) / static_cast<float>(width_), 0.0f, 1.0f);
    metrics.bbox_bottom = std::clamp(
        static_cast<float>(max_y_ + 1) / static_cast<float>(height_), 0.0f, 1.0f);
    metrics.bbox_area_ratio = std::clamp(
        (metrics.bbox_right - metrics.bbox_left) *
            (metrics.bbox_bottom - metrics.bbox_top),
        0.0f, 1.0f);

    if (metrics.centroid_x < 1.0f / 3.0f) {
        metrics.horizontal_region = CameraMotionHorizontalRegion::kLeft;
    } else if (metrics.centroid_x >= 2.0f / 3.0f) {
        metrics.horizontal_region = CameraMotionHorizontalRegion::kRight;
    } else {
        metrics.horizontal_region = CameraMotionHorizontalRegion::kCenter;
    }
    if (metrics.centroid_y < 1.0f / 3.0f) {
        metrics.vertical_region = CameraMotionVerticalRegion::kTop;
    } else if (metrics.centroid_y >= 2.0f / 3.0f) {
        metrics.vertical_region = CameraMotionVerticalRegion::kBottom;
    } else {
        metrics.vertical_region = CameraMotionVerticalRegion::kCenter;
    }
    return metrics;
}

const char* CameraMotionTracker::HorizontalRegionName(
    CameraMotionHorizontalRegion region) {
    switch (region) {
        case CameraMotionHorizontalRegion::kLeft: return "left";
        case CameraMotionHorizontalRegion::kCenter: return "center";
        case CameraMotionHorizontalRegion::kRight: return "right";
        case CameraMotionHorizontalRegion::kNone:
        default: return "none";
    }
}

const char* CameraMotionTracker::VerticalRegionName(
    CameraMotionVerticalRegion region) {
    switch (region) {
        case CameraMotionVerticalRegion::kTop: return "top";
        case CameraMotionVerticalRegion::kCenter: return "center";
        case CameraMotionVerticalRegion::kBottom: return "bottom";
        case CameraMotionVerticalRegion::kNone:
        default: return "none";
    }
}
