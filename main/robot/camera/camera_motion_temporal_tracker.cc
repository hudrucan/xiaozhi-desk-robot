#include "camera_motion_temporal_tracker.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int64_t kHistoryStaleMs = 2500;
constexpr uint32_t kMinimumTrendSpanMs = 150;
constexpr float kDirectionDeadbandPerSec = 0.08f;
constexpr float kDominantAxisRatio = 1.35f;
constexpr float kLeftZoneMaximum = 0.45f;
constexpr float kRightZoneMinimum = 0.55f;

}  // namespace

void CameraMotionTemporalTracker::Reset() { ResetHistory(); }

CameraMotionTemporalResult CameraMotionTemporalTracker::Observe(
    int64_t timestamp_ms, const CameraMotionSpatialMetrics& spatial) {
    result_.crossed_center = false;
    result_.crossing = CameraMotionCrossing::kNone;
    if (!spatial.valid) {
        return Advance(timestamp_ms);
    }
    if (IsStale(timestamp_ms)) {
        ResetHistory();
    }
    Append({timestamp_ms, spatial.centroid_x, spatial.centroid_y,
            spatial.bbox_area_ratio});
    UpdateCrossing(spatial.centroid_x, timestamp_ms);
    UpdateTrend();
    return result_;
}

CameraMotionTemporalResult CameraMotionTemporalTracker::Advance(
    int64_t timestamp_ms) {
    result_.crossed_center = false;
    result_.crossing = CameraMotionCrossing::kNone;
    if (IsStale(timestamp_ms)) {
        ResetHistory();
    }
    return result_;
}

void CameraMotionTemporalTracker::ResetHistory() {
    history_count_ = 0;
    crossing_origin_ = HorizontalZone::kNeutral;
    entered_neutral_ = false;
    result_ = {};
    result_.crossing_count = crossing_count_;
    result_.last_crossing = last_crossing_;
    result_.last_crossing_ms = last_crossing_ms_;
}

void CameraMotionTemporalTracker::Append(const Sample& sample) {
    if (history_count_ < history_.size()) {
        history_[history_count_++] = sample;
        return;
    }
    for (size_t index = 1; index < history_.size(); ++index) {
        history_[index - 1] = history_[index];
    }
    history_.back() = sample;
}

void CameraMotionTemporalTracker::UpdateCrossing(float centroid_x,
                                                 int64_t timestamp_ms) {
    HorizontalZone zone = HorizontalZone::kNeutral;
    if (centroid_x < kLeftZoneMaximum) {
        zone = HorizontalZone::kLeft;
    } else if (centroid_x > kRightZoneMinimum) {
        zone = HorizontalZone::kRight;
    }

    if (zone == HorizontalZone::kNeutral) {
        if (crossing_origin_ != HorizontalZone::kNeutral) {
            entered_neutral_ = true;
        }
        return;
    }
    if (crossing_origin_ == HorizontalZone::kNeutral) {
        crossing_origin_ = zone;
        entered_neutral_ = false;
        return;
    }
    if (zone == crossing_origin_) {
        entered_neutral_ = false;
        return;
    }
    if (entered_neutral_) {
        result_.crossed_center = true;
        result_.crossing = crossing_origin_ == HorizontalZone::kLeft
                               ? CameraMotionCrossing::kLeftToRight
                               : CameraMotionCrossing::kRightToLeft;
        ++crossing_count_;
        last_crossing_ = result_.crossing;
        last_crossing_ms_ = timestamp_ms;
        result_.crossing_count = crossing_count_;
        result_.last_crossing = last_crossing_;
        result_.last_crossing_ms = last_crossing_ms_;
    }
    crossing_origin_ = zone;
    entered_neutral_ = false;
}

void CameraMotionTemporalTracker::UpdateTrend() {
    result_.valid = false;
    result_.sample_count = static_cast<uint8_t>(history_count_);
    result_.span_ms = history_count_ > 1
                          ? static_cast<uint32_t>(std::max<int64_t>(
                                0, history_[history_count_ - 1].timestamp_ms -
                                       history_[0].timestamp_ms))
                          : 0;
    result_.velocity_x_per_sec = 0.0f;
    result_.velocity_y_per_sec = 0.0f;
    result_.direction = CameraMotionDirection::kNone;
    result_.crossing_count = crossing_count_;
    if (history_count_ < 3 || result_.span_ms < kMinimumTrendSpanMs) {
        return;
    }

    float mean_time_sec = 0.0f;
    float mean_x = 0.0f;
    float mean_y = 0.0f;
    for (size_t index = 0; index < history_count_; ++index) {
        mean_time_sec += static_cast<float>(
            history_[index].timestamp_ms - history_[0].timestamp_ms) / 1000.0f;
        mean_x += history_[index].centroid_x;
        mean_y += history_[index].centroid_y;
    }
    const float count = static_cast<float>(history_count_);
    mean_time_sec /= count;
    mean_x /= count;
    mean_y /= count;

    float time_variance = 0.0f;
    float covariance_x = 0.0f;
    float covariance_y = 0.0f;
    for (size_t index = 0; index < history_count_; ++index) {
        const float time_sec = static_cast<float>(
            history_[index].timestamp_ms - history_[0].timestamp_ms) / 1000.0f;
        const float centered_time = time_sec - mean_time_sec;
        time_variance += centered_time * centered_time;
        covariance_x += centered_time * (history_[index].centroid_x - mean_x);
        covariance_y += centered_time * (history_[index].centroid_y - mean_y);
    }
    if (time_variance <= 0.000001f) {
        return;
    }

    result_.valid = true;
    result_.velocity_x_per_sec = covariance_x / time_variance;
    result_.velocity_y_per_sec = covariance_y / time_variance;
    const float absolute_x = std::fabs(result_.velocity_x_per_sec);
    const float absolute_y = std::fabs(result_.velocity_y_per_sec);
    if (absolute_x >= kDirectionDeadbandPerSec &&
        absolute_x >= absolute_y * kDominantAxisRatio) {
        result_.direction = result_.velocity_x_per_sec < 0.0f
                                ? CameraMotionDirection::kLeft
                                : CameraMotionDirection::kRight;
    } else if (absolute_y >= kDirectionDeadbandPerSec &&
               absolute_y >= absolute_x * kDominantAxisRatio) {
        result_.direction = result_.velocity_y_per_sec < 0.0f
                                ? CameraMotionDirection::kUp
                                : CameraMotionDirection::kDown;
    }
}

bool CameraMotionTemporalTracker::IsStale(int64_t timestamp_ms) const {
    if (history_count_ == 0) {
        return false;
    }
    const int64_t elapsed_ms =
        timestamp_ms - history_[history_count_ - 1].timestamp_ms;
    return elapsed_ms <= 0 || elapsed_ms > kHistoryStaleMs;
}

const char* CameraMotionTemporalTracker::DirectionName(
    CameraMotionDirection direction) {
    switch (direction) {
        case CameraMotionDirection::kLeft: return "left";
        case CameraMotionDirection::kRight: return "right";
        case CameraMotionDirection::kUp: return "up";
        case CameraMotionDirection::kDown: return "down";
        case CameraMotionDirection::kNone:
        default: return "none";
    }
}

const char* CameraMotionTemporalTracker::CrossingName(
    CameraMotionCrossing crossing) {
    switch (crossing) {
        case CameraMotionCrossing::kLeftToRight: return "left_to_right";
        case CameraMotionCrossing::kRightToLeft: return "right_to_left";
        case CameraMotionCrossing::kNone:
        default: return "none";
    }
}
