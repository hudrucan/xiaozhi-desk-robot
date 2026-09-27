#pragma once

#include <cstdint>

class CameraVisualWakeDetector {
public:
    enum class Stage : uint8_t { kArmed, kSawUp, kSawDown, kTriggered };

    bool Observe(float x, float y, int64_t sample_ms);
    void Advance(int64_t now_ms);
    void ResetSequence();
    Stage stage() const { return stage_; }
    int64_t stage_started_ms() const { return stage_started_ms_; }
    uint32_t wake_count() const { return wake_count_; }

    static const char* StageName(Stage stage);

private:
    static constexpr float kLegExcursion = 0.13f;
    static constexpr float kMaximumXDrift = 0.45f;
    static constexpr int64_t kGestureTimeoutMs = 1700;
    static constexpr int64_t kStaleSampleMs = 650;

    Stage stage_ = Stage::kArmed;
    bool have_anchor_ = false;
    float anchor_x_ = 0.0f;
    float anchor_y_ = 0.0f;
    float turning_y_ = 0.0f;
    int64_t gesture_started_ms_ = 0;
    int64_t stage_started_ms_ = 0;
    int64_t last_sample_ms_ = 0;
    uint32_t wake_count_ = 0;
};
