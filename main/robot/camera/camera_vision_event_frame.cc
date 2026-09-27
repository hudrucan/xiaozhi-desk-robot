#include "camera_vision_event_frame.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "CameraEventFrame"

CameraVisionEventFrame::~CameraVisionEventFrame() { Release(); }

void CameraVisionEventFrame::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled == status_.enabled &&
        ((!enabled && frame_ == nullptr) || (enabled && frame_ != nullptr))) {
        return;
    }
    if (enabled && frame_ == nullptr) {
        frame_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            16, kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (frame_ == nullptr) {
            ESP_LOGW(TAG, "Unable to allocate motion event frame buffer");
        }
    } else if (!enabled && frame_ != nullptr) {
        heap_caps_free(frame_);
        frame_ = nullptr;
    }
    status_.enabled = enabled;
    status_.available = false;
    status_.frame_ms = 0;
    status_.psram_bytes = frame_ != nullptr ? kBytes : 0;
    status_.source = CameraVisionEventFrameSource::kNone;
    status_.spatial = {};
}

bool CameraVisionEventFrame::Capture(const uint8_t* rgb565, size_t width,
                                     size_t height, size_t stride,
                                     CameraVisionEventFrameSource source,
                                     const CameraMotionSpatialMetrics& spatial) {
    if (rgb565 == nullptr || width != kWidth || height != kHeight ||
        stride < kWidth * sizeof(uint16_t) ||
        source == CameraVisionEventFrameSource::kNone) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !status_.enabled || frame_ == nullptr) {
        return false;
    }
    constexpr size_t kRowBytes = kWidth * sizeof(uint16_t);
    if (stride == kRowBytes) {
        std::memcpy(frame_, rgb565, kBytes);
    } else {
        for (size_t row = 0; row < kHeight; ++row) {
            std::memcpy(frame_ + row * kRowBytes, rgb565 + row * stride,
                        kRowBytes);
        }
    }
    status_.available = true;
    status_.frame_ms = esp_timer_get_time() / 1000;
    status_.source = source;
    status_.spatial = spatial;
    return true;
}

bool CameraVisionEventFrame::Send(const FrameSender& sender) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_ != nullptr && status_.available &&
           sender(frame_, kBytes, status_.spatial);
}

CameraVisionEventFrameStatus CameraVisionEventFrame::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void CameraVisionEventFrame::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.available = false;
    status_.frame_ms = 0;
    status_.source = CameraVisionEventFrameSource::kNone;
    status_.spatial = {};
}

void CameraVisionEventFrame::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_ != nullptr) {
        heap_caps_free(frame_);
        frame_ = nullptr;
    }
    status_ = {};
}

const char* CameraVisionEventFrame::SourceName(
    CameraVisionEventFrameSource source) {
    switch (source) {
        case CameraVisionEventFrameSource::kActivity: return "activity";
        case CameraVisionEventFrameSource::kMotion: return "motion";
        case CameraVisionEventFrameSource::kManual: return "manual";
        case CameraVisionEventFrameSource::kNone:
        default: return "none";
    }
}
