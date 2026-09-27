#pragma once

#include "camera_motion_tracker.h"

#include <array>
#include <cstddef>
#include <cstdint>

enum class CameraMotionDirection : uint8_t {
    kNone,
    kLeft,
    kRight,
    kUp,
    kDown,
};

enum class CameraMotionCrossing : uint8_t {
    kNone,
    kLeftToRight,
    kRightToLeft,
};

struct CameraMotionTemporalResult {
    bool valid = false;
    uint8_t sample_count = 0;
    uint32_t span_ms = 0;
    float velocity_x_per_sec = 0.0f;
    float velocity_y_per_sec = 0.0f;
    CameraMotionDirection direction = CameraMotionDirection::kNone;
    bool crossed_center = false;
    CameraMotionCrossing crossing = CameraMotionCrossing::kNone;
    uint32_t crossing_count = 0;
    CameraMotionCrossing last_crossing = CameraMotionCrossing::kNone;
    int64_t last_crossing_ms = 0;
};

class CameraMotionTemporalTracker {
public:
    static constexpr size_t kHistoryCapacity = 6;

    void Reset();
    CameraMotionTemporalResult Observe(
        int64_t timestamp_ms, const CameraMotionSpatialMetrics& spatial);
    CameraMotionTemporalResult Advance(int64_t timestamp_ms);
    CameraMotionTemporalResult GetResult() const { return result_; }

    static const char* DirectionName(CameraMotionDirection direction);
    static const char* CrossingName(CameraMotionCrossing crossing);

private:
    struct Sample {
        int64_t timestamp_ms = 0;
        float centroid_x = 0.0f;
        float centroid_y = 0.0f;
        float bbox_area_ratio = 0.0f;
    };

    enum class HorizontalZone : uint8_t {
        kNeutral,
        kLeft,
        kRight,
    };

    void ResetHistory();
    void Append(const Sample& sample);
    void UpdateCrossing(float centroid_x, int64_t timestamp_ms);
    void UpdateTrend();
    bool IsStale(int64_t timestamp_ms) const;

    std::array<Sample, kHistoryCapacity> history_{};
    size_t history_count_ = 0;
    HorizontalZone crossing_origin_ = HorizontalZone::kNeutral;
    bool entered_neutral_ = false;
    uint32_t crossing_count_ = 0;
    CameraMotionCrossing last_crossing_ = CameraMotionCrossing::kNone;
    int64_t last_crossing_ms_ = 0;
    CameraMotionTemporalResult result_;
};
