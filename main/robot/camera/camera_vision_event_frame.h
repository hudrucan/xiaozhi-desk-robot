#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

enum class CameraVisionEventFrameSource : uint8_t {
    kNone,
    kMotion,
    kManual,
};

struct CameraVisionEventFrameStatus {
    bool enabled = false;
    bool available = false;
    bool capture_pending = false;
    int64_t frame_ms = 0;
    size_t psram_bytes = 0;
    CameraVisionEventFrameSource source = CameraVisionEventFrameSource::kNone;
};

class CameraVisionEventFrame {
public:
    static constexpr size_t kWidth = 160;
    static constexpr size_t kHeight = 120;
    static constexpr size_t kBytes = kWidth * kHeight * sizeof(uint16_t);
    using FrameSender = std::function<bool(const uint8_t*, size_t)>;

    CameraVisionEventFrame() = default;
    ~CameraVisionEventFrame();

    CameraVisionEventFrame(const CameraVisionEventFrame&) = delete;
    CameraVisionEventFrame& operator=(const CameraVisionEventFrame&) = delete;

    void SetEnabled(bool enabled);
    bool Capture(const uint8_t* rgb565, size_t width, size_t height,
                 size_t stride, CameraVisionEventFrameSource source);
    bool Send(const FrameSender& sender) const;
    CameraVisionEventFrameStatus GetStatus() const;
    void Clear();
    void Release();

    static const char* SourceName(CameraVisionEventFrameSource source);

private:
    mutable std::mutex mutex_;
    uint8_t* frame_ = nullptr;
    CameraVisionEventFrameStatus status_;
};
