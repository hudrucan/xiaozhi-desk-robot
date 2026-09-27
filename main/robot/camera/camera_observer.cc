#include "camera_observer.h"

#include <algorithm>
#include <cmath>

#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace {

bool VisionSettingsEqual(const CameraVisionSettings& lhs,
                         const CameraVisionSettings& rhs) {
    return lhs.enabled == rhs.enabled &&
           lhs.quiet_interval_ms == rhs.quiet_interval_ms &&
           lhs.active_interval_ms == rhs.active_interval_ms &&
           lhs.cell_threshold == rhs.cell_threshold &&
           lhs.activity_ratio_bp == rhs.activity_ratio_bp &&
           lhs.enter_ratio_bp == rhs.enter_ratio_bp &&
           lhs.exit_ratio_bp == rhs.exit_ratio_bp &&
           lhs.enter_samples == rhs.enter_samples &&
           lhs.exit_samples == rhs.exit_samples &&
           lhs.activity_hold_ms == rhs.activity_hold_ms;
}

}  // namespace

CameraObserver::~CameraObserver() {
    if (scratch_ != nullptr) {
        heap_caps_free(scratch_);
    }
}

void CameraObserver::Configure(const CameraVisionSettings& settings) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    const bool enabling = settings.enabled && !status_.enabled;
    const bool reset_temporal_for_settings =
        settings.enabled && status_.enabled &&
        !VisionSettingsEqual(settings_, settings);
    settings_ = settings;
    status_.enabled = settings.enabled;
    status_.interval_ms = settings.quiet_interval_ms;
    status_.current_interval_ms =
        status_.sampling_state == CameraObserverSamplingState::kQuiet
            ? settings.quiet_interval_ms
            : settings.active_interval_ms;
    if (!settings.enabled) {
        status_.state = CameraObserverState::kDisabled;
        status_.suspend_reason = CameraObserverSuspendReason::kDisabled;
        status_.motion_active = false;
        status_.sampling_state = CameraObserverSamplingState::kQuiet;
        status_.current_interval_ms = settings.quiet_interval_ms;
        have_previous_ = false;
        high_motion_samples_ = 0;
        low_motion_samples_ = 0;
        burst_deadline_ms_ = 0;
        ResetTemporalLocked();
    } else if (enabling || status_.state == CameraObserverState::kDisabled) {
        status_.state = CameraObserverState::kWaiting;
        status_.suspend_reason = CameraObserverSuspendReason::kWaitingFirstSample;
        status_.motion_active = false;
        status_.sampling_state = CameraObserverSamplingState::kQuiet;
        status_.current_interval_ms = settings.quiet_interval_ms;
        have_previous_ = false;
        high_motion_samples_ = 0;
        low_motion_samples_ = 0;
        burst_deadline_ms_ = 0;
        ResetTemporalLocked();
    } else if (reset_temporal_for_settings) {
        ResetTemporalLocked();
    }
}

bool CameraObserver::EnsureScratch() {
    if (scratch_ != nullptr) {
        return true;
    }
    scratch_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, kDecodeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.scratch_psram_bytes = scratch_ != nullptr ? kDecodeBytes : 0;
    return scratch_ != nullptr;
}

void CameraObserver::SetState(CameraObserverState state,
                              CameraObserverSuspendReason reason,
                              bool count_skip) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = status_.enabled ? state : CameraObserverState::kDisabled;
    status_.suspend_reason = status_.enabled ? reason
                                             : CameraObserverSuspendReason::kDisabled;
    if (state != CameraObserverState::kActive &&
        state != CameraObserverState::kWaiting) {
        ResetTemporalLocked();
    }
    if (status_.enabled && count_skip) {
        ++status_.skipped_count;
    }
}

void CameraObserver::RecordFailure(CameraObserverSuspendReason reason) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = status_.enabled ? CameraObserverState::kError
                                    : CameraObserverState::kDisabled;
    status_.suspend_reason = status_.enabled ? reason
                                             : CameraObserverSuspendReason::kDisabled;
    ResetTemporalLocked();
    if (status_.enabled) {
        ++status_.failure_count;
    }
}

void CameraObserver::ResetBaseline() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.motion_active = false;
    status_.sampling_state = CameraObserverSamplingState::kQuiet;
    status_.current_interval_ms = settings_.quiet_interval_ms;
    have_previous_ = false;
    high_motion_samples_ = 0;
    low_motion_samples_ = 0;
    burst_deadline_ms_ = 0;
    ResetTemporalLocked();
}

void CameraObserver::ResetTemporalLocked() {
    temporal_tracker_.Reset();
    status_.temporal = temporal_tracker_.GetResult();
}

uint8_t CameraObserver::Rgb565Luma(uint16_t pixel) {
    const uint32_t red = ((pixel >> 11) & 0x1F) * 255 / 31;
    const uint32_t green = ((pixel >> 5) & 0x3F) * 255 / 63;
    const uint32_t blue = (pixel & 0x1F) * 255 / 31;
    return static_cast<uint8_t>((red * 77 + green * 150 + blue * 29) >> 8);
}

CameraObserverAnalysisResult CameraObserver::ProcessRgb565(
    size_t width, size_t height, size_t stride, uint32_t capture_ms,
    uint32_t decode_ms, int64_t sample_start_us, bool update_motion_state) {
    CameraObserverAnalysisResult result;
    if (scratch_ == nullptr || width != kDecodeWidth || height != kDecodeHeight ||
        stride < kDecodeStride) {
        RecordFailure(CameraObserverSuspendReason::kInvalidFrame);
        return result;
    }

    CameraVisionSettings settings;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        settings = settings_;
        result.sampling_state = status_.sampling_state;
        result.temporal = status_.temporal;
    }
    const int64_t analyze_start_us = esp_timer_get_time();
    uint32_t luma_sum = 0;
    for (size_t grid_y = 0; grid_y < kGridHeight; ++grid_y) {
        for (size_t grid_x = 0; grid_x < kGridWidth; ++grid_x) {
            uint32_t cell_sum = 0;
            for (size_t pixel_y = 0; pixel_y < 4; ++pixel_y) {
                const auto* row = reinterpret_cast<const uint16_t*>(
                    scratch_ + (grid_y * 4 + pixel_y) * stride);
                for (size_t pixel_x = 0; pixel_x < 4; ++pixel_x) {
                    cell_sum += Rgb565Luma(row[grid_x * 4 + pixel_x]);
                }
            }
            const uint8_t cell_luma = static_cast<uint8_t>(cell_sum / 16);
            current_luma_[grid_y * kGridWidth + grid_x] = cell_luma;
            luma_sum += cell_luma;
        }
    }

    const float global_luma = static_cast<float>(luma_sum) / kGridCells;
    float motion_score = 0.0f;
    float center_score = 0.0f;
    float changed_ratio = 0.0f;
    CameraMotionSpatialMetrics spatial;
    if (have_previous_) {
        uint32_t previous_sum = 0;
        for (const uint8_t value : previous_luma_) {
            previous_sum += value;
        }
        const float previous_mean = static_cast<float>(previous_sum) / kGridCells;
        const float global_delta = global_luma - previous_mean;
        float motion_sum = 0.0f;
        float center_sum = 0.0f;
        size_t center_cells = 0;
        motion_tracker_.BeginFrame(kGridWidth, kGridHeight,
                                   settings.cell_threshold);
        for (size_t y = 0; y < kGridHeight; ++y) {
            for (size_t x = 0; x < kGridWidth; ++x) {
                const size_t index = y * kGridWidth + x;
                const float difference = std::fabs(
                    (static_cast<float>(current_luma_[index]) - previous_luma_[index]) -
                    global_delta);
                motion_sum += difference;
                motion_tracker_.AddCell(x, y, difference);
                if (x >= kGridWidth / 4 && x < (kGridWidth * 3) / 4 &&
                    y >= kGridHeight / 4 && y < (kGridHeight * 3) / 4) {
                    center_sum += difference;
                    ++center_cells;
                }
            }
        }
        motion_score = motion_sum / kGridCells;
        center_score = center_cells > 0 ? center_sum / center_cells : 0.0f;
        spatial = motion_tracker_.FinishFrame();
        changed_ratio = static_cast<float>(spatial.active_cells) / kGridCells;
    }

    // Manual event-frame capture may publish telemetry, but it must not become
    // part of the adaptive sampler's baseline or state machine.
    if (update_motion_state) {
        previous_luma_ = current_luma_;
    }
    const int64_t analyze_end_us = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (update_motion_state) {
        have_previous_ = true;
        const int64_t now_ms = analyze_end_us / 1000;
        const int changed_ratio_bp = static_cast<int>(
            spatial.active_cells * 10000 / kGridCells);
        if (status_.motion_active) {
            status_.sampling_state = CameraObserverSamplingState::kMotion;
            high_motion_samples_ = 0;
            if (changed_ratio_bp <= settings.exit_ratio_bp) {
                ++low_motion_samples_;
                if (low_motion_samples_ >= settings.exit_samples) {
                    status_.motion_active = false;
                    low_motion_samples_ = 0;
                    result.motion_exited = true;
                    if (changed_ratio_bp >= settings.activity_ratio_bp) {
                        status_.sampling_state = CameraObserverSamplingState::kBurst;
                        burst_deadline_ms_ = now_ms + settings.activity_hold_ms;
                    } else {
                        status_.sampling_state = CameraObserverSamplingState::kQuiet;
                        burst_deadline_ms_ = 0;
                    }
                }
            } else {
                low_motion_samples_ = 0;
            }
        } else {
            low_motion_samples_ = 0;
            if (changed_ratio_bp >= settings.activity_ratio_bp) {
                status_.sampling_state = CameraObserverSamplingState::kBurst;
                burst_deadline_ms_ = now_ms + settings.activity_hold_ms;
            } else if (status_.sampling_state == CameraObserverSamplingState::kBurst &&
                       now_ms >= burst_deadline_ms_) {
                status_.sampling_state = CameraObserverSamplingState::kQuiet;
                burst_deadline_ms_ = 0;
            }
            if (changed_ratio_bp >= settings.enter_ratio_bp) {
                ++high_motion_samples_;
                if (high_motion_samples_ >= settings.enter_samples) {
                    status_.motion_active = true;
                    status_.sampling_state = CameraObserverSamplingState::kMotion;
                    burst_deadline_ms_ = 0;
                    ++status_.motion_event_count;
                    status_.last_motion_event_ms = now_ms;
                    high_motion_samples_ = 0;
                    result.motion_entered = true;
                }
            } else {
                high_motion_samples_ = 0;
            }
        }
        status_.current_interval_ms =
            status_.sampling_state == CameraObserverSamplingState::kQuiet
                ? settings.quiet_interval_ms
                : settings.active_interval_ms;
        if (status_.sampling_state == CameraObserverSamplingState::kQuiet) {
            ResetTemporalLocked();
        } else if (spatial.valid) {
            status_.temporal = temporal_tracker_.Observe(now_ms, spatial);
        } else {
            status_.temporal = temporal_tracker_.Advance(now_ms);
        }
        result.sampling_state = status_.sampling_state;
        result.temporal = status_.temporal;
    }

    if (status_.enabled) {
        status_.state = CameraObserverState::kActive;
        status_.suspend_reason = CameraObserverSuspendReason::kNone;
    }
    status_.last_sample_ms = analyze_end_us / 1000;
    status_.global_luma = global_luma;
    status_.motion_score = motion_score;
    status_.center_activity_score = center_score;
    status_.changed_pixel_ratio = changed_ratio;
    status_.spatial = spatial;
    status_.capture_ms = capture_ms;
    status_.decode_ms = decode_ms;
    status_.analyze_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, (analyze_end_us - analyze_start_us + 999) / 1000));
    status_.total_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, (analyze_end_us - sample_start_us + 999) / 1000));
    ++status_.sample_count;
    result.valid = true;
    result.spatial = spatial;
    return result;
}

CameraObserverStatus CameraObserver::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

int CameraObserver::GetRecommendedIntervalMs() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_.current_interval_ms;
}

const char* CameraObserver::StateName(CameraObserverState state) {
    switch (state) {
        case CameraObserverState::kWaiting: return "waiting";
        case CameraObserverState::kActive: return "active";
        case CameraObserverState::kSuspendedNonIdle: return "suspended_non_idle";
        case CameraObserverState::kSuspendedExplicitCamera:
            return "suspended_explicit_camera";
        case CameraObserverState::kSuspendedMcp: return "suspended_mcp";
        case CameraObserverState::kBusy: return "busy";
        case CameraObserverState::kError: return "error";
        case CameraObserverState::kDisabled:
        default: return "disabled";
    }
}

const char* CameraObserver::SuspendReasonName(CameraObserverSuspendReason reason) {
    switch (reason) {
        case CameraObserverSuspendReason::kWaitingFirstSample: return "waiting_first_sample";
        case CameraObserverSuspendReason::kNonIdle: return "non_idle";
        case CameraObserverSuspendReason::kWebLive: return "web_live";
        case CameraObserverSuspendReason::kMochanPreview: return "mochan_preview";
        case CameraObserverSuspendReason::kMcp: return "mcp";
        case CameraObserverSuspendReason::kCameraBusy: return "camera_busy";
        case CameraObserverSuspendReason::kScratchUnavailable: return "scratch_unavailable";
        case CameraObserverSuspendReason::kCaptureFailed: return "capture_failed";
        case CameraObserverSuspendReason::kDecodeFailed: return "decode_failed";
        case CameraObserverSuspendReason::kInvalidFrame: return "invalid_frame";
        case CameraObserverSuspendReason::kDisabled: return "disabled";
        case CameraObserverSuspendReason::kNone:
        default: return "none";
    }
}

const char* CameraObserver::SamplingStateName(CameraObserverSamplingState state) {
    switch (state) {
        case CameraObserverSamplingState::kBurst: return "burst";
        case CameraObserverSamplingState::kMotion: return "motion";
        case CameraObserverSamplingState::kQuiet:
        default: return "quiet";
    }
}
