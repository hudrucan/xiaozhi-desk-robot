#pragma once

#include <esp_timer.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class ReactionEngine {
public:
    enum class Source : uint8_t { kExternal, kCamera, kMpu, kProactive, kAmbient };
    enum class MotionState : uint8_t {
        kNotRequested,
        kPending,
        kStarted,
        kSkippedDisabled,
        kSkippedBusy,
        kSkippedUnsafe,
        kRejected,
        kCompleted,
        kCanceled,
        kSuperseded,
    };

    struct Plan {
        std::string name;
        std::string emotion;
        std::string light_effect;
        std::string motion_profile;
        int priority = 0;
    };

    struct Status {
        bool active = false;
        std::string name;
        int priority = 0;
        uint32_t generation = 0;
        int remaining_ms = 0;
        MotionState motion = MotionState::kNotRequested;
        bool oled_active = false;
    };

    struct Callbacks {
        std::function<void(const Plan&, const std::string&, int, uint32_t, bool)> apply;
        std::function<void(uint32_t)> release;
    };

    ReactionEngine() = default;
    ~ReactionEngine();

    bool Initialize(Callbacks callbacks);
    bool Start(const std::string& name, int duration_ms, const std::string& oled_text,
               Source source = Source::kExternal, uint32_t* generation_out = nullptr);
    bool Cancel();
    bool CancelIfGeneration(uint32_t expected_generation);
    Status GetStatus() const;

    void SetMotionState(uint32_t generation, MotionState state);
    void SetOledActive(uint32_t generation, bool active);
    bool IsActive() const;
    bool IsActiveGeneration(uint32_t generation) const;

    static bool IsSupported(const std::string& name);
    static const char* MotionStateName(MotionState state);

private:
    static bool ResolvePlan(const std::string& name, Plan& plan);
    static void TimerCallback(void* arg);
    void HandleTimer();

    mutable std::mutex mutex_;
    Callbacks callbacks_;
    esp_timer_handle_t timer_ = nullptr;
    bool initialized_ = false;
    bool active_ = false;
    Plan plan_;
    Source source_ = Source::kExternal;
    uint32_t generation_ = 0;
    int64_t expires_at_us_ = 0;
    MotionState motion_state_ = MotionState::kNotRequested;
    bool oled_active_ = false;
};
