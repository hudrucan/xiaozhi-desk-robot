#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "board.h"
#include "display.h"
#include "esp32_camera.h"
#include "jpg/jpeg_to_image.h"
#include "lvgl_display.h"

#define TAG "Esp32Camera"

namespace {

constexpr size_t kPreviewDecodeMaxWidth = 320;
constexpr size_t kPreviewDecodeMaxHeight = 240;

void GetPreviewDecodeSize(size_t source_width, size_t source_height,
                          size_t& target_width, size_t& target_height) {
    target_width = 0;
    target_height = 0;
    if (source_width <= kPreviewDecodeMaxWidth && source_height <= kPreviewDecodeMaxHeight) {
        return;
    }
    const double width_scale = static_cast<double>(kPreviewDecodeMaxWidth) / source_width;
    const double height_scale = static_cast<double>(kPreviewDecodeMaxHeight) / source_height;
    const double scale = width_scale < height_scale ? width_scale : height_scale;
    target_width = std::max<size_t>(8, (static_cast<size_t>(source_width * scale) / 8) * 8);
    target_height = std::max<size_t>(8, (static_cast<size_t>(source_height * scale) / 8) * 8);
    // esp_new_jpeg supports downscaling to at most 1/8 of the source.
    const size_t minimum_width = ((source_width + 63) / 64) * 8;
    const size_t minimum_height = ((source_height + 63) / 64) * 8;
    target_width = std::max(target_width, minimum_width);
    target_height = std::max(target_height, minimum_height);
}

}  // namespace

OwnedJpeg::~OwnedJpeg() { Reset(); }

OwnedJpeg::OwnedJpeg(OwnedJpeg&& other) noexcept
    : data(std::exchange(other.data, nullptr)),
      length(std::exchange(other.length, 0)),
      width(std::exchange(other.width, 0)),
      height(std::exchange(other.height, 0)) {}

OwnedJpeg& OwnedJpeg::operator=(OwnedJpeg&& other) noexcept {
    if (this != &other) {
        Reset();
        data = std::exchange(other.data, nullptr);
        length = std::exchange(other.length, 0);
        width = std::exchange(other.width, 0);
        height = std::exchange(other.height, 0);
    }
    return *this;
}

bool OwnedJpeg::CopyFrom(const camera_fb_t& frame) {
    Reset();
    if (frame.format != PIXFORMAT_JPEG || frame.buf == nullptr || frame.len == 0) {
        return false;
    }

    data = static_cast<uint8_t*>(
        heap_caps_malloc(frame.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        data = static_cast<uint8_t*>(heap_caps_malloc(frame.len, MALLOC_CAP_8BIT));
    }
    if (data == nullptr) {
        return false;
    }

    memcpy(data, frame.buf, frame.len);
    length = frame.len;
    width = frame.width;
    height = frame.height;
    return true;
}

void OwnedJpeg::Reset() {
    if (data != nullptr) {
        heap_caps_free(data);
    }
    data = nullptr;
    length = 0;
    width = 0;
    height = 0;
}

#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
#if CONFIG_XIAOZHI_CAMERA_HMIRROR
static constexpr bool kConfiguredHMirror = true;
#else
static constexpr bool kConfiguredHMirror = false;
#endif
#if CONFIG_XIAOZHI_CAMERA_VFLIP
static constexpr bool kConfiguredVFlip = true;
#else
static constexpr bool kConfiguredVFlip = false;
#endif
#endif

Esp32Camera::Esp32Camera(const camera_config_t& config, std::mutex* shared_i2c_mutex)
    : shared_i2c_mutex_(shared_i2c_mutex) {
    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
    }
    constexpr std::array<int, 3> kProbeRetryDelayMs = {0, 100, 250};
    esp_err_t err = ESP_FAIL;
    for (size_t attempt = 0; attempt < kProbeRetryDelayMs.size(); ++attempt) {
        if (kProbeRetryDelayMs[attempt] > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kProbeRetryDelayMs[attempt]));
        }
        err = esp_camera_init(&config);
        if (err == ESP_OK) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "Camera initialized after %zu probe attempts", attempt + 1);
            }
            break;
        }
        ESP_LOGW(TAG, "esp_camera_init attempt %zu/%zu failed: %s (0x%x)", attempt + 1,
                 kProbeRetryDelayMs.size(), esp_err_to_name(err), err);
        if (err != ESP_ERR_NOT_SUPPORTED) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0);  // Control camera mirror: 1 for mirror, 0 for normal
        }
#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED
        s->set_hmirror(s, kConfiguredHMirror ? 1 : 0);
        s->set_vflip(s, kConfiguredVFlip ? 1 : 0);
#endif
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }

    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
    if (streaming_on_) {
        ReturnCurrentFrame();
        if (encode_buf_) {
            heap_caps_free(encode_buf_);
            encode_buf_ = nullptr;
            encode_buf_size_ = 0;
        }
        std::unique_lock<std::mutex> i2c_lock;
        if (shared_i2c_mutex_ != nullptr) {
            i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
        }
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() { return CaptureInternal(true, 1); }

bool Esp32Camera::CaptureOwnedJpeg(int warmup_frames) {
    // Show the captured frame once as five-second user feedback. This does not
    // make MCP a persistent preview mode; its camera ownership remains a
    // transient still operation and the framebuffer is returned below.
    if (!CaptureInternal(true, warmup_frames)) {
        return false;
    }

    OwnedJpeg snapshot;
    const bool copied = current_fb_ != nullptr && snapshot.CopyFrom(*current_fb_);
    ReturnCurrentFrame();
    if (!copied) {
        ESP_LOGE(TAG, "Failed to copy MCP JPEG into owned memory");
        return false;
    }

    const int width = snapshot.width;
    const int height = snapshot.height;
    const size_t length = snapshot.length;
    {
        std::lock_guard<std::mutex> lock(mcp_snapshot_mutex_);
        mcp_snapshot_ = std::move(snapshot);
    }
    ESP_LOGI(TAG, "MCP JPEG copied: %dx%d, len=%zu", width, height, length);
    return true;
}

bool Esp32Camera::CaptureForPreview() { return CaptureInternal(true, 0); }

bool Esp32Camera::CaptureForWeb() { return CaptureInternal(false, 0); }

bool Esp32Camera::GetCurrentJpeg(const uint8_t*& data, size_t& length) const {
    if (current_fb_ == nullptr || current_fb_->format != PIXFORMAT_JPEG) {
        data = nullptr;
        length = 0;
        return false;
    }
    data = current_fb_->buf;
    length = current_fb_->len;
    return data != nullptr && length > 0;
}

bool Esp32Camera::CaptureInternal(bool update_preview, int discard_frames) {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (!streaming_on_) {
        return false;
    }

    // Preview consumers use the driver's latest framebuffer directly. A still
    // capture may discard transition frames so AEC/AGC can converge after a
    // profile or resolution switch.
    const int capture_count = std::max(0, discard_frames) + 1;
    for (int i = 0; i < capture_count; i++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // Prepare encode buffer for RGB565 format (with optional byte swapping)
    if (update_preview && current_fb_->format == PIXFORMAT_RGB565) {
        size_t pixel_count = current_fb_->width * current_fb_->height;
        size_t data_size = pixel_count * 2;

        // Allocate or reallocate encode buffer if needed
        if (encode_buf_size_ < data_size) {
            if (encode_buf_) {
                heap_caps_free(encode_buf_);
            }
            encode_buf_ =
                (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (encode_buf_ == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for encode buffer");
                encode_buf_size_ = 0;
                return false;
            }
            encode_buf_size_ = data_size;
        }

        // Copy data to encode buffer with optional byte swapping
        uint16_t* src = (uint16_t*)current_fb_->buf;
        uint16_t* dst = (uint16_t*)encode_buf_;
        if (swap_bytes_enabled_) {
            for (size_t i = 0; i < pixel_count; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
        } else {
            memcpy(encode_buf_, current_fb_->buf, data_size);
        }

        // Allocate separate buffer for preview display
        uint8_t* preview_data =
            (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (preview_data != nullptr) {
            memcpy(preview_data, encode_buf_, data_size);
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    preview_data, data_size, current_fb_->width, current_fb_->height,
                    current_fb_->width * 2, LV_COLOR_FORMAT_RGB565));
            } else {
                heap_caps_free(preview_data);
            }
        }
    } else if (update_preview && current_fb_->format == PIXFORMAT_JPEG) {
        uint8_t* preview_data = nullptr;
        size_t preview_len = 0;
        size_t preview_width = 0;
        size_t preview_height = 0;
        size_t preview_stride = 0;
        size_t decode_width = 0;
        size_t decode_height = 0;
        GetPreviewDecodeSize(current_fb_->width, current_fb_->height,
                             decode_width, decode_height);
        esp_err_t decode_result =
            jpeg_to_image_scaled(current_fb_->buf, current_fb_->len, &preview_data, &preview_len,
                                 &preview_width, &preview_height, &preview_stride,
                                 decode_width, decode_height);
        if (decode_result == ESP_OK) {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    preview_data, preview_len, preview_width, preview_height, preview_stride,
                    LV_COLOR_FORMAT_RGB565));
            } else {
                heap_caps_free(preview_data);
            }
        } else {
            ESP_LOGE(TAG, "JPEG preview decode failed: %s", esp_err_to_name(decode_result));
        }
    }

    ESP_LOGD(TAG, "Captured frame: %dx%d, len=%zu, format=%d", current_fb_->width,
             current_fb_->height, current_fb_->len, current_fb_->format);

    return true;
}

void Esp32Camera::ReturnCurrentFrame() {
    if (current_fb_ != nullptr) {
        esp_camera_fb_return(current_fb_);
        current_fb_ = nullptr;
    }
}

bool Esp32Camera::SetHMirror(bool enabled) {
    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
    }
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
    }
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

bool Esp32Camera::ApplySensorControls(const CameraSensorControls& controls) {
    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
    }
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        return false;
    }

    int result = 0;
    result |= sensor->set_brightness(sensor, controls.brightness);
    result |= sensor->set_contrast(sensor, controls.contrast);
    result |= sensor->set_saturation(sensor, controls.saturation);
    result |= sensor->set_exposure_ctrl(sensor, controls.auto_exposure ? 1 : 0);
    result |= sensor->set_aec2(sensor, controls.aec2 ? 1 : 0);
    result |= sensor->set_ae_level(sensor, controls.ae_level);
    if (!controls.auto_exposure) {
        result |= sensor->set_aec_value(sensor, controls.manual_exposure);
    }
    result |= sensor->set_gain_ctrl(sensor, controls.auto_gain ? 1 : 0);
    result |= sensor->set_gainceiling(sensor, controls.gain_ceiling);
    if (!controls.auto_gain) {
        result |= sensor->set_agc_gain(sensor, controls.manual_gain);
    }
    result |= sensor->set_whitebal(sensor, controls.auto_white_balance ? 1 : 0);
    result |= sensor->set_awb_gain(sensor, controls.awb_gain ? 1 : 0);
    result |= sensor->set_wb_mode(sensor, controls.white_balance_mode);
    result |= sensor->set_bpc(sensor, controls.black_pixel_correction ? 1 : 0);
    result |= sensor->set_wpc(sensor, controls.white_pixel_correction ? 1 : 0);
    result |= sensor->set_raw_gma(sensor, controls.gamma ? 1 : 0);
    result |= sensor->set_lenc(sensor, controls.lens_correction ? 1 : 0);
    result |= sensor->set_hmirror(sensor, controls.mirror ? 1 : 0);
    result |= sensor->set_vflip(sensor, controls.flip ? 1 : 0);
    return result == 0;
}

bool Esp32Camera::ApplyCaptureSettings(framesize_t frame_size, int jpeg_quality) {
    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
    }
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        return false;
    }
    // Reprogramming the OV2640 frame size resets its DVP/pixformat path and
    // temporarily destabilizes AEC/AGC. Avoid doing that at every still capture
    // when the requested capture mode is already active.
    const int frame_result = sensor->status.framesize == frame_size
                                 ? 0
                                 : sensor->set_framesize(sensor, frame_size);
    const int quality_result = sensor->status.quality == jpeg_quality
                                   ? 0
                                   : sensor->set_quality(sensor, jpeg_quality);
    return frame_result == 0 && quality_result == 0;
}

int Esp32Camera::SensorPid() const {
    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_);
    }
    sensor_t* sensor = esp_camera_sensor_get();
    return sensor != nullptr ? sensor->id.PID : 0;
}
