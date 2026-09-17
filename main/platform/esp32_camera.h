#pragma once
#include "sdkconfig.h"

#include <lvgl.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"
#include "esp_camera.h"
#include "jpg/image_to_jpeg.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

struct OwnedJpeg {
    uint8_t* data = nullptr;
    size_t length = 0;
    int width = 0;
    int height = 0;

    OwnedJpeg() = default;
    ~OwnedJpeg();
    OwnedJpeg(const OwnedJpeg&) = delete;
    OwnedJpeg& operator=(const OwnedJpeg&) = delete;
    OwnedJpeg(OwnedJpeg&& other) noexcept;
    OwnedJpeg& operator=(OwnedJpeg&& other) noexcept;

    bool CopyFrom(const camera_fb_t& frame);
    void Reset();
    explicit operator bool() const { return data != nullptr && length > 0; }
};

class Esp32Camera : public Camera {
private:
    bool streaming_on_ = false;
    bool swap_bytes_enabled_ = true;  // Swap pixel byte order for RGB565, enabled by default
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    camera_fb_t* current_fb_ = nullptr;
    uint8_t* encode_buf_ = nullptr;  // Buffer for JPEG encoding (with optional byte swap)
    size_t encode_buf_size_ = 0;
    std::mutex mcp_snapshot_mutex_;
    OwnedJpeg mcp_snapshot_;

public:
    Esp32Camera(const camera_config_t& config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token) override;
    virtual bool Capture() override;
    bool CaptureForWeb();
    bool GetCurrentJpeg(const uint8_t*& data, size_t& length) const;
    bool IsAvailable() const { return streaming_on_; }
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual bool SetSwapBytes(bool enabled) override;
    virtual std::expected<std::string, std::string> Explain(const std::string& question) override;

protected:
    bool CaptureOwnedJpeg();

private:
    bool CaptureInternal(bool update_preview);
    void ReturnCurrentFrame();
    void LogHttpDiagnostics(const char* stage, int64_t request_start_us) const;
};
