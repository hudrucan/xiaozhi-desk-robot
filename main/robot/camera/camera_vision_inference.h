#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

class HumanFaceDetect;

enum class CameraFaceInferenceState : uint8_t {
    kDisabled,
    kIdle,
    kLoading,
    kReady,
    kRunning,
    kError,
};

enum class CameraFaceRegion : uint8_t {
    kNone,
    kLeft,
    kCenter,
    kRight,
};

struct CameraVisionInferenceStatus {
    bool enabled = false;
    CameraFaceInferenceState state = CameraFaceInferenceState::kDisabled;
    bool model_loaded = false;
    int64_t last_inference_ms = 0;
    uint32_t inference_count = 0;
    uint32_t failure_count = 0;
    bool face_present = false;
    uint32_t face_count = 0;
    float best_confidence = 0.0f;
    int best_box_x1 = 0;
    int best_box_y1 = 0;
    int best_box_x2 = 0;
    int best_box_y2 = 0;
    CameraFaceRegion face_region = CameraFaceRegion::kNone;
    uint32_t init_ms = 0;
    uint32_t inference_ms = 0;
    size_t internal_free_before_init = 0;
    size_t internal_free_after_init = 0;
    int64_t internal_init_delta = 0;
    size_t internal_largest_before_init = 0;
    size_t internal_largest_after_init = 0;
    size_t psram_free_before_init = 0;
    size_t psram_free_after_init = 0;
    int64_t psram_init_delta = 0;
};

class CameraVisionInference {
public:
    CameraVisionInference();
    ~CameraVisionInference();

    CameraVisionInference(const CameraVisionInference&) = delete;
    CameraVisionInference& operator=(const CameraVisionInference&) = delete;

    // Lifecycle calls are serialized by DeskRobotCamera's observer sample gate.
    void Configure(bool enabled);
    void Unload();
    bool RunRgb565(const uint8_t* data, size_t width, size_t height,
                   size_t stride);
    CameraVisionInferenceStatus GetStatus() const;

    static const char* StateName(CameraFaceInferenceState state);
    static const char* RegionName(CameraFaceRegion region);

private:
    void RecordFailure(uint32_t inference_ms = 0, bool record_time = false);
    void ClearCurrentDetectionLocked();

    mutable std::mutex status_mutex_;
    CameraVisionInferenceStatus status_;
    std::unique_ptr<HumanFaceDetect> detector_;
};
