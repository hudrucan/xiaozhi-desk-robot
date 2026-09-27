#include "camera_observer.h"

#include "config/tuning.h"

#include <algorithm>
#include <cmath>

#include <esp_heap_caps.h>
#include <esp_timer.h>

CameraObserver::~CameraObserver() {
    if (scratch_ != nullptr) {
        heap_caps_free(scratch_);
    }
}

void CameraObserver::Configure(bool enabled, int interval_ms) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    const bool enabling = enabled && !status_.enabled;
    status_.enabled = enabled;
    status_.interval_ms = interval_ms;
    if (!enabled) {
        status_.state = CameraObserverState::kDisabled;
        status_.suspend_reason = CameraObserverSuspendReason::kDisabled;
        status_.motion_active = false;
        have_previous_ = false;
        high_motion_samples_ = 0;
        low_motion_samples_ = 0;
    } else if (enabling || status_.state == CameraObserverState::kDisabled) {
        status_.state = CameraObserverState::kWaiting;
        status_.suspend_reason = CameraObserverSuspendReason::kWaitingFirstSample;
        status_.motion_active = false;
        have_previous_ = false;
        high_motion_samples_ = 0;
        low_motion_samples_ = 0;
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
    if (status_.enabled) {
        ++status_.failure_count;
    }
}

void CameraObserver::ResetBaseline() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.motion_active = false;
    have_previous_ = false;
    high_motion_samples_ = 0;
    low_motion_samples_ = 0;
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
    if (have_previous_) {
        uint32_t previous_sum = 0;
        for (const uint8_t value : previous_luma_) {
            previous_sum += value;
        }
        const float previous_mean = static_cast<float>(previous_sum) / kGridCells;
        const float global_delta = global_luma - previous_mean;
        uint32_t changed_cells = 0;
        float motion_sum = 0.0f;
        float center_sum = 0.0f;
        size_t center_cells = 0;
        for (size_t y = 0; y < kGridHeight; ++y) {
            for (size_t x = 0; x < kGridWidth; ++x) {
                const size_t index = y * kGridWidth + x;
                const float difference = std::fabs(
                    (static_cast<float>(current_luma_[index]) - previous_luma_[index]) -
                    global_delta);
                motion_sum += difference;
                if (difference >= CAMERA_OBSERVER_CELL_MOTION_THRESHOLD) {
                    ++changed_cells;
                }
                if (x >= kGridWidth / 4 && x < (kGridWidth * 3) / 4 &&
                    y >= kGridHeight / 4 && y < (kGridHeight * 3) / 4) {
                    center_sum += difference;
                    ++center_cells;
                }
            }
        }
        motion_score = motion_sum / kGridCells;
        center_score = center_cells > 0 ? center_sum / center_cells : 0.0f;
        changed_ratio = static_cast<float>(changed_cells) / kGridCells;
    }

    if (update_motion_state) {
        previous_luma_ = current_luma_;
    }
    const int64_t analyze_end_us = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (update_motion_state) {
        have_previous_ = true;
        if (status_.motion_active) {
            high_motion_samples_ = 0;
            if (changed_ratio <= CAMERA_OBSERVER_MOTION_EXIT_RATIO) {
                ++low_motion_samples_;
                if (low_motion_samples_ >= CAMERA_OBSERVER_MOTION_EXIT_SAMPLES) {
                    status_.motion_active = false;
                    low_motion_samples_ = 0;
                    result.motion_exited = true;
                }
            } else {
                low_motion_samples_ = 0;
            }
        } else {
            low_motion_samples_ = 0;
            if (changed_ratio >= CAMERA_OBSERVER_MOTION_ENTER_RATIO) {
                ++high_motion_samples_;
                if (high_motion_samples_ >= CAMERA_OBSERVER_MOTION_ENTER_SAMPLES) {
                    status_.motion_active = true;
                    ++status_.motion_event_count;
                    status_.last_motion_event_ms = analyze_end_us / 1000;
                    high_motion_samples_ = 0;
                    result.motion_entered = true;
                }
            } else {
                high_motion_samples_ = 0;
            }
        }
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
    status_.capture_ms = capture_ms;
    status_.decode_ms = decode_ms;
    status_.analyze_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, (analyze_end_us - analyze_start_us + 999) / 1000));
    status_.total_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, (analyze_end_us - sample_start_us + 999) / 1000));
    ++status_.sample_count;
    result.valid = true;
    return result;
}

CameraObserverStatus CameraObserver::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
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
