#pragma once

#include "camera_motion_temporal_tracker.h"
#include "camera_motion_tracker.h"
#include "camera_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

enum class CameraObserverState : uint8_t {
    kDisabled,
    kWaiting,
    kActive,
    kSuspendedNonIdle,
    kSuspendedExplicitCamera,
    kSuspendedMcp,
    kBusy,
    kError,
};

enum class CameraObserverSuspendReason : uint8_t {
    kNone,
    kDisabled,
    kWaitingFirstSample,
    kNonIdle,
    kWebLive,
    kMochanPreview,
    kMcp,
    kCameraBusy,
    kScratchUnavailable,
    kCaptureFailed,
    kDecodeFailed,
    kInvalidFrame,
};

enum class CameraObserverSamplingState : uint8_t {
    kQuiet,
    kBurst,
    kMotion,
};

struct CameraObserverStatus {
    bool enabled = false;
    CameraObserverState state = CameraObserverState::kDisabled;
    CameraObserverSuspendReason suspend_reason = CameraObserverSuspendReason::kDisabled;
    int interval_ms = 1000;
    CameraObserverSamplingState sampling_state = CameraObserverSamplingState::kQuiet;
    int current_interval_ms = 1000;
    int64_t last_sample_ms = 0;
    float global_luma = 0.0f;
    float motion_score = 0.0f;
    float center_activity_score = 0.0f;
    float changed_pixel_ratio = 0.0f;
    bool activity_active = false;
    CameraMotionSpatialMetrics spatial;
    CameraMotionTemporalResult temporal;
    bool motion_active = false;
    uint32_t motion_event_count = 0;
    int64_t last_motion_event_ms = 0;
    uint32_t capture_ms = 0;
    uint32_t decode_ms = 0;
    uint32_t analyze_ms = 0;
    uint32_t total_ms = 0;
    uint32_t sample_count = 0;
    uint32_t normal_sample_count = 0;
    uint32_t skipped_count = 0;
    uint32_t failure_count = 0;
    size_t scratch_psram_bytes = 0;
};

struct CameraObserverAnalysisResult {
    bool valid = false;
    bool motion_entered = false;
    bool motion_exited = false;
    CameraObserverSamplingState sampling_state = CameraObserverSamplingState::kQuiet;
    CameraMotionSpatialMetrics spatial;
    CameraMotionTemporalResult temporal;
};

class CameraObserver {
public:
    static constexpr size_t kDecodeWidth = 160;
    static constexpr size_t kDecodeHeight = 120;
    static constexpr size_t kDecodeStride = kDecodeWidth * 2;
    static constexpr size_t kDecodeBytes = kDecodeStride * kDecodeHeight;
    static constexpr size_t kGridWidth = 40;
    static constexpr size_t kGridHeight = 30;
    static constexpr size_t kGridCells = kGridWidth * kGridHeight;

    CameraObserver() = default;
    ~CameraObserver();

    void Configure(const CameraVisionSettings& settings);
    bool EnsureScratch();
    uint8_t* scratch_data() { return scratch_; }
    size_t scratch_capacity() const { return scratch_ != nullptr ? kDecodeBytes : 0; }
    void SetState(CameraObserverState state, CameraObserverSuspendReason reason,
                  bool count_skip = false);
    void RecordFailure(CameraObserverSuspendReason reason);
    void ResetBaseline();
    CameraObserverAnalysisResult ProcessRgb565(
        size_t width, size_t height, size_t stride, uint32_t capture_ms,
        uint32_t decode_ms, int64_t sample_start_us,
        bool update_motion_state = true);
    CameraObserverStatus GetStatus() const;
    int GetRecommendedIntervalMs() const;

    static const char* StateName(CameraObserverState state);
    static const char* SuspendReasonName(CameraObserverSuspendReason reason);
    static const char* SamplingStateName(CameraObserverSamplingState state);

private:
    static uint8_t Rgb565Luma(uint16_t pixel);
    void ResetTemporalLocked();

    mutable std::mutex status_mutex_;
    CameraObserverStatus status_;
    CameraVisionSettings settings_;
    uint8_t* scratch_ = nullptr;
    std::array<uint8_t, kGridCells> previous_luma_{};
    std::array<uint8_t, kGridCells> current_luma_{};
    CameraMotionTracker motion_tracker_;
    CameraMotionTemporalTracker temporal_tracker_;
    bool have_previous_ = false;
    uint8_t high_motion_samples_ = 0;
    uint8_t low_motion_samples_ = 0;
    int64_t burst_deadline_ms_ = 0;
};
