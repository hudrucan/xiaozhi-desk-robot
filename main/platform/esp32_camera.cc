#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
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
constexpr int kOv5640GainCeilingRegister = 0x3A18;
constexpr int kOv5640GainCeilingMask = 0x03FF;
constexpr int kOv5640CcmFirstRegister = 0x5381;
constexpr int kOv5640CcmRegisterMask = 0xFF;
constexpr int kOv5640AwbTableFirstRegister = 0x5180;
constexpr int kOv5640AwbTableLastRegister = 0x519E;
constexpr size_t kOv5640AwbTableSize =
    kOv5640AwbTableLastRegister - kOv5640AwbTableFirstRegister + 1;
constexpr size_t kOv5640AdvancedAwbIndex = 0x5183 - kOv5640AwbTableFirstRegister;

uint32_t HashRegisterBytes(const uint8_t* bytes, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return hash;
}

std::array<uint8_t, kOv5640AwbTableSize> NormalizeAwbCalibrationTable(
    const std::array<uint8_t, kOv5640AwbTableSize>& table) {
    auto normalized = table;
    // 0x5183 bit 7 selects Advanced/Simple AWB and is runtime mode state,
    // not calibration data. Preserve the other seven calibration bits.
    normalized[kOv5640AdvancedAwbIndex] &= 0x7F;
    return normalized;
}

int ApplyGainCeiling(sensor_t* sensor, gainceiling_t ceiling) {
    if (sensor->id.PID != OV5640_PID) {
        return sensor->set_gainceiling(sensor, ceiling);
    }

    // OV5640 encodes the AGC ceiling in 1/16x units in the 10-bit
    // 0x3A18/0x3A19 field. esp32-camera 2.1.7 writes the gainceiling_t enum
    // ordinal directly, which turns e.g. 8x into raw value 2 and leaves auto
    // gain far darker than requested. Keep the workaround in application code
    // until the component fixes its OV5640 implementation.
    const int ordinal = std::clamp(static_cast<int>(ceiling),
                                   static_cast<int>(GAINCEILING_2X),
                                   static_cast<int>(GAINCEILING_128X));
    const int multiplier = 2 << ordinal;
    const int raw_ceiling = std::min(multiplier << 4, kOv5640GainCeilingMask);
    const int result = sensor->set_reg(sensor, kOv5640GainCeilingRegister,
                                       kOv5640GainCeilingMask, raw_ceiling);
    if (result == 0) {
        sensor->status.gainceiling = static_cast<uint8_t>(ceiling);
    }
    return result;
}

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
        CaptureOv5640NativeCcm(s);
        CaptureOv5640NativeAwbTable(s);
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

bool Esp32Camera::CaptureOwnedJpeg(int warmup_frames, size_t* captured_size) {
    if (captured_size != nullptr) {
        *captured_size = 0;
    }
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
    if (captured_size != nullptr) {
        *captured_size = length;
    }
    ESP_LOGD(TAG, "MCP JPEG copied: %dx%d, len=%zu", width, height, length);
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

void Esp32Camera::CaptureOv5640NativeCcm(sensor_t* sensor) {
    if (sensor->id.PID != OV5640_PID) {
        return;
    }

    for (size_t index = 0; index < ov5640_native_ccm_.size(); ++index) {
        const int value = sensor->get_reg(
            sensor, kOv5640CcmFirstRegister + static_cast<int>(index),
            kOv5640CcmRegisterMask);
        if (value < 0) {
            ESP_LOGW(TAG,
                     "Cannot capture native OV5640 CCM at 0x%04x; neutral saturation "
                     "will use the driver fallback",
                     kOv5640CcmFirstRegister + static_cast<int>(index));
            ov5640_native_ccm_valid_ = false;
            return;
        }
        ov5640_native_ccm_[index] = static_cast<uint8_t>(value);
    }
    ov5640_native_ccm_valid_ = true;
    ESP_LOGI(TAG, "Captured native OV5640 neutral CCM");
}

void Esp32Camera::CaptureOv5640NativeAwbTable(sensor_t* sensor) {
    if (sensor->id.PID != OV5640_PID) {
        return;
    }

    for (size_t index = 0; index < ov5640_native_awb_table_.size(); ++index) {
        const int value = sensor->get_reg(
            sensor, kOv5640AwbTableFirstRegister + static_cast<int>(index),
            kOv5640CcmRegisterMask);
        if (value < 0) {
            ESP_LOGW(TAG, "Cannot capture native OV5640 AWB table at 0x%04x",
                     kOv5640AwbTableFirstRegister + static_cast<int>(index));
            ov5640_native_awb_table_valid_ = false;
            return;
        }
        ov5640_native_awb_table_[index] = static_cast<uint8_t>(value);
    }
    const auto normalized = NormalizeAwbCalibrationTable(ov5640_native_awb_table_);
    ov5640_native_awb_table_hash_ =
        HashRegisterBytes(normalized.data(), normalized.size());
    ov5640_native_awb_table_valid_ = true;
    ESP_LOGI(TAG, "Captured native OV5640 AWB calibration table");
}

bool Esp32Camera::ReadDiagnostics(CameraDiagnostics& diagnostics) {
    CameraDiagnostics snapshot;
    sensor_t* sensor = esp_camera_sensor_get();
    snapshot.available = sensor != nullptr && sensor->id.PID == OV5640_PID;
    if (!snapshot.available) {
        diagnostics = snapshot;
        return false;
    }

    std::unique_lock<std::mutex> i2c_lock;
    if (shared_i2c_mutex_ != nullptr) {
        i2c_lock = std::unique_lock<std::mutex>(*shared_i2c_mutex_, std::try_to_lock);
        if (!i2c_lock.owns_lock()) {
            return false;
        }
    }

    const int64_t read_start_us = esp_timer_get_time();
    auto read8 = [sensor](int address, uint8_t& output) {
        const int value = sensor->get_reg(sensor, address, 0xFF);
        if (value < 0) {
            return false;
        }
        output = static_cast<uint8_t>(value);
        return true;
    };
    auto read16 = [&read8](int address, uint16_t& output) {
        uint8_t high = 0;
        uint8_t low = 0;
        if (!read8(address, high) || !read8(address + 1, low)) {
            return false;
        }
        output = static_cast<uint16_t>((static_cast<uint16_t>(high) << 8) | low);
        return true;
    };

    snapshot.awb_enabled = sensor->status.awb;
    snapshot.awb_gain_enabled = sensor->status.awb_gain;
    snapshot.advanced_awb_enabled = sensor->status.dcw;
    snapshot.wb_mode = sensor->status.wb_mode;
    snapshot.aec_enabled = sensor->status.aec;
    snapshot.aec2_enabled = sensor->status.aec2;
    snapshot.agc_enabled = sensor->status.agc;
    snapshot.bpc_enabled = sensor->status.bpc;
    snapshot.wpc_enabled = sensor->status.wpc;
    snapshot.gamma_enabled = sensor->status.raw_gma;
    snapshot.lens_correction_enabled = sensor->status.lenc;
    snapshot.mirror_enabled = sensor->status.hmirror;
    snapshot.flip_enabled = sensor->status.vflip;

    uint8_t exposure_high = 0;
    uint8_t exposure_middle = 0;
    uint8_t exposure_low = 0;
    uint8_t gain_high = 0;
    uint8_t gain_low = 0;
    if (!read8(0x3406, snapshot.wb_control_raw) ||
        !read16(0x3400, snapshot.awb_r_gain_raw) ||
        !read16(0x3402, snapshot.awb_g_gain_raw) ||
        !read16(0x3404, snapshot.awb_b_gain_raw) ||
        !read8(0x5000, snapshot.isp_control_00_raw) ||
        !read8(0x5001, snapshot.isp_control_01_raw) ||
        !read8(0x3500, exposure_high) ||
        !read8(0x3501, exposure_middle) ||
        !read8(0x3502, exposure_low) ||
        !read8(0x350A, gain_high) ||
        !read8(0x350B, gain_low) ||
        !read16(kOv5640GainCeilingRegister, snapshot.gain_ceiling_raw) ||
        !read8(0x3A0F, snapshot.ae_target_high) ||
        !read8(0x3A10, snapshot.ae_target_low) ||
        !read8(0x3A1B, snapshot.ae_target_high_2) ||
        !read8(0x3A1E, snapshot.ae_target_low_2) ||
        !read8(0x3A11, snapshot.ae_fast_high) ||
        !read8(0x3A1F, snapshot.ae_fast_low)) {
        return false;
    }
    snapshot.awb_r_gain_raw &= 0x0FFF;
    snapshot.awb_g_gain_raw &= 0x0FFF;
    snapshot.awb_b_gain_raw &= 0x0FFF;

    snapshot.exposure_raw =
        (static_cast<uint32_t>(exposure_high & 0x0F) << 12) |
        (static_cast<uint32_t>(exposure_middle) << 4) |
        ((exposure_low & 0xF0) >> 4);
    snapshot.gain_raw = static_cast<uint8_t>(((gain_low & 0xF0) >> 4) |
                                             ((gain_high & 0x03) << 4));
    if ((gain_low & 0x0F) != 0) {
        ++snapshot.gain_raw;
    }
    snapshot.gain_ceiling_raw &= kOv5640GainCeilingMask;

    for (size_t index = 0; index < snapshot.ccm_current.size(); ++index) {
        if (!read8(kOv5640CcmFirstRegister + static_cast<int>(index),
                   snapshot.ccm_current[index])) {
            return false;
        }
    }
    snapshot.ccm_native = ov5640_native_ccm_;
    snapshot.ccm_native_valid = ov5640_native_ccm_valid_;
    snapshot.ccm_matches_native =
        snapshot.ccm_native_valid && snapshot.ccm_current == snapshot.ccm_native;

    std::array<uint8_t, kOv5640AwbTableSize> current_awb_table{};
    for (size_t index = 0; index < current_awb_table.size(); ++index) {
        if (!read8(kOv5640AwbTableFirstRegister + static_cast<int>(index),
                   current_awb_table[index])) {
            return false;
        }
    }
    const auto normalized_current = NormalizeAwbCalibrationTable(current_awb_table);
    const auto normalized_native =
        NormalizeAwbCalibrationTable(ov5640_native_awb_table_);
    snapshot.awb_table_current_hash =
        HashRegisterBytes(normalized_current.data(), normalized_current.size());
    snapshot.awb_table_native_hash = ov5640_native_awb_table_hash_;
    snapshot.awb_table_native_valid = ov5640_native_awb_table_valid_;
    snapshot.awb_table_matches_native =
        snapshot.awb_table_native_valid &&
        normalized_current == normalized_native;

    const int64_t read_end_us = esp_timer_get_time();
    snapshot.register_read_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, (read_end_us - read_start_us + 999) / 1000));
    snapshot.last_read_ms = read_end_us / 1000;
    snapshot.valid = true;
    diagnostics = snapshot;
    return true;
}

int Esp32Camera::ApplySaturation(sensor_t* sensor, int saturation) {
    if (sensor->id.PID != OV5640_PID || saturation != 0 ||
        !ov5640_native_ccm_valid_) {
        return sensor->set_saturation(sensor, saturation);
    }

    for (size_t index = 0; index < ov5640_native_ccm_.size(); ++index) {
        const int result = sensor->set_reg(
            sensor, kOv5640CcmFirstRegister + static_cast<int>(index),
            kOv5640CcmRegisterMask, ov5640_native_ccm_[index]);
        if (result != 0) {
            return result;
        }
    }
    sensor->status.saturation = 0;
    return 0;
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
    result |= ApplySaturation(sensor, controls.saturation);
    result |= sensor->set_exposure_ctrl(sensor, controls.auto_exposure ? 1 : 0);
    result |= sensor->set_aec2(sensor, controls.aec2 ? 1 : 0);
    result |= sensor->set_ae_level(sensor, controls.ae_level);
    if (!controls.auto_exposure) {
        result |= sensor->set_aec_value(sensor, controls.manual_exposure);
    }
    result |= sensor->set_gain_ctrl(sensor, controls.auto_gain ? 1 : 0);
    result |= ApplyGainCeiling(sensor, controls.gain_ceiling);
    if (!controls.auto_gain) {
        result |= sensor->set_agc_gain(sensor, controls.manual_gain);
    }
    result |= sensor->set_whitebal(sensor, controls.auto_white_balance ? 1 : 0);
    result |= sensor->set_awb_gain(sensor, controls.awb_gain ? 1 : 0);
    result |= sensor->set_wb_mode(sensor, controls.white_balance_mode);
    result |= sensor->set_dcw(sensor, controls.advanced_awb ? 1 : 0);
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
    // Reprogramming the sensor frame size temporarily destabilizes AEC/AGC.
    // Avoid doing that at every still capture when the requested mode is
    // already active.
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
