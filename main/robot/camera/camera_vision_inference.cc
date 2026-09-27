#include "camera_vision_inference.h"

#include "human_face_detect.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "CameraFace"

CameraVisionInference::CameraVisionInference() = default;

CameraVisionInference::~CameraVisionInference() = default;

void CameraVisionInference::Configure(bool enabled) {
    if (!enabled) {
        detector_.reset();
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.enabled = false;
        status_.state = CameraFaceInferenceState::kDisabled;
        status_.model_loaded = false;
        ClearCurrentDetectionLocked();
        return;
    }

    std::lock_guard<std::mutex> lock(status_mutex_);
    if (!status_.enabled) {
        status_.enabled = true;
        status_.state = CameraFaceInferenceState::kIdle;
        status_.model_loaded = false;
        ClearCurrentDetectionLocked();
    }
}

void CameraVisionInference::Unload() {
    detector_.reset();
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.model_loaded = false;
    status_.state = status_.enabled ? CameraFaceInferenceState::kIdle
                                    : CameraFaceInferenceState::kDisabled;
    ClearCurrentDetectionLocked();
}

bool CameraVisionInference::RunRgb565(const uint8_t* data, size_t width,
                                      size_t height, size_t stride) {
    if (data == nullptr || width == 0 || height == 0 ||
        stride != width * sizeof(uint16_t) ||
        width > std::numeric_limits<uint16_t>::max() ||
        height > std::numeric_limits<uint16_t>::max()) {
        RecordFailure();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (!status_.enabled) {
            return false;
        }
    }

    if (detector_ == nullptr) {
        const size_t internal_before =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest_before =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t psram_before =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const int64_t init_start_us = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.state = CameraFaceInferenceState::kLoading;
        }

        std::unique_ptr<HumanFaceDetect> detector(new (std::nothrow) HumanFaceDetect(
            HumanFaceDetect::MSRMNP_S8_V1, false));
        const int64_t init_end_us = esp_timer_get_time();
        const size_t internal_after =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest_after =
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t psram_after =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.init_ms = static_cast<uint32_t>(std::max<int64_t>(
                0, (init_end_us - init_start_us + 999) / 1000));
            status_.internal_free_before_init = internal_before;
            status_.internal_free_after_init = internal_after;
            status_.internal_init_delta =
                static_cast<int64_t>(internal_after) -
                static_cast<int64_t>(internal_before);
            status_.internal_largest_before_init = largest_before;
            status_.internal_largest_after_init = largest_after;
            status_.psram_free_before_init = psram_before;
            status_.psram_free_after_init = psram_after;
            status_.psram_init_delta =
                static_cast<int64_t>(psram_after) -
                static_cast<int64_t>(psram_before);
        }
        if (detector == nullptr) {
            ESP_LOGE(TAG, "Unable to allocate HumanFaceDetect");
            RecordFailure();
            return false;
        }
        detector_ = std::move(detector);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.model_loaded = true;
            status_.state = CameraFaceInferenceState::kReady;
        }
        ESP_LOGI(TAG, "HumanFaceDetect MSR+MNP loaded in %u ms",
                 GetStatus().init_ms);
    }

    dl::image::img_t image = {
        .data = const_cast<uint8_t*>(data),
        .width = static_cast<uint16_t>(width),
        .height = static_cast<uint16_t>(height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = CameraFaceInferenceState::kRunning;
        ++status_.inference_count;
    }
    const int64_t inference_start_us = esp_timer_get_time();
    const auto& detections = detector_->run(image);
    const int64_t inference_end_us = esp_timer_get_time();
    const uint32_t inference_ms = static_cast<uint32_t>(std::max<int64_t>(
        0, (inference_end_us - inference_start_us + 999) / 1000));

    float best_confidence = -std::numeric_limits<float>::infinity();
    const dl::detect::result_t* best = nullptr;
    for (const auto& detection : detections) {
        if (detection.box.size() < 4 || !std::isfinite(detection.score) ||
            detection.box[0] < 0 || detection.box[1] < 0 ||
            detection.box[2] < detection.box[0] ||
            detection.box[3] < detection.box[1] ||
            detection.box[2] >= static_cast<int>(width) ||
            detection.box[3] >= static_cast<int>(height)) {
            RecordFailure(inference_ms, true);
            return false;
        }
        if (best == nullptr || detection.score > best_confidence) {
            best = &detection;
            best_confidence = detection.score;
        }
    }

    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = CameraFaceInferenceState::kReady;
    status_.last_inference_ms = inference_end_us / 1000;
    status_.inference_ms = inference_ms;
    status_.face_present = best != nullptr;
    status_.face_count = static_cast<uint32_t>(detections.size());
    status_.best_confidence = best != nullptr ? best_confidence : 0.0f;
    status_.face_region = CameraFaceRegion::kNone;
    status_.best_box_x1 = 0;
    status_.best_box_y1 = 0;
    status_.best_box_x2 = 0;
    status_.best_box_y2 = 0;
    if (best != nullptr) {
        status_.best_box_x1 = best->box[0];
        status_.best_box_y1 = best->box[1];
        status_.best_box_x2 = best->box[2];
        status_.best_box_y2 = best->box[3];
        const int center_x = (best->box[0] + best->box[2]) / 2;
        if (center_x < static_cast<int>(width / 3)) {
            status_.face_region = CameraFaceRegion::kLeft;
        } else if (center_x >= static_cast<int>((width * 2) / 3)) {
            status_.face_region = CameraFaceRegion::kRight;
        } else {
            status_.face_region = CameraFaceRegion::kCenter;
        }
    }
    return true;
}

CameraVisionInferenceStatus CameraVisionInference::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

const char* CameraVisionInference::StateName(CameraFaceInferenceState state) {
    switch (state) {
        case CameraFaceInferenceState::kIdle: return "idle";
        case CameraFaceInferenceState::kLoading: return "loading";
        case CameraFaceInferenceState::kReady: return "ready";
        case CameraFaceInferenceState::kRunning: return "running";
        case CameraFaceInferenceState::kError: return "error";
        case CameraFaceInferenceState::kDisabled:
        default: return "disabled";
    }
}

const char* CameraVisionInference::RegionName(CameraFaceRegion region) {
    switch (region) {
        case CameraFaceRegion::kLeft: return "left";
        case CameraFaceRegion::kCenter: return "center";
        case CameraFaceRegion::kRight: return "right";
        case CameraFaceRegion::kNone:
        default: return "none";
    }
}

void CameraVisionInference::RecordFailure(uint32_t inference_ms,
                                          bool record_time) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.state = status_.enabled ? CameraFaceInferenceState::kError
                                    : CameraFaceInferenceState::kDisabled;
    status_.model_loaded = detector_ != nullptr;
    status_.inference_ms = inference_ms;
    if (record_time) {
        status_.last_inference_ms = esp_timer_get_time() / 1000;
    }
    ClearCurrentDetectionLocked();
    ++status_.failure_count;
}

void CameraVisionInference::ClearCurrentDetectionLocked() {
    status_.face_present = false;
    status_.face_count = 0;
    status_.best_confidence = 0.0f;
    status_.best_box_x1 = 0;
    status_.best_box_y1 = 0;
    status_.best_box_x2 = 0;
    status_.best_box_y2 = 0;
    status_.face_region = CameraFaceRegion::kNone;
}
