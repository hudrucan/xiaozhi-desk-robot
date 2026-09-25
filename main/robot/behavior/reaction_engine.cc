#include "reaction_engine.h"

#include <algorithm>
#include <array>
#include <utility>

namespace {

struct ReactionDefinition {
    const char* name;
    const char* emotion;
    const char* light_effect;
    const char* motion_emotion;
    int priority;
};

constexpr std::array<ReactionDefinition, 12> kDefinitions = {{
    {"acknowledge", "happy", "steady", "", 40},
    {"success", "happy", "steady", "happy", 60},
    {"celebrate", "laughing", "blink", "laughing", 70},
    {"thinking", "thinking", "breathe", "", 30},
    {"curious", "suspicious", "breathe", "thinking", 35},
    {"confused", "confused", "blink", "confused", 45},
    {"warning", "shocked", "blink", "", 90},
    {"error", "angry", "blink", "", 100},
    {"startled", "surprised", "blink", "surprised", 80},
    {"sleepy", "sleepy", "breathe", "", 20},
    {"nope", "shake", "blink", "shake", 50},
    {"attention", "surprised", "steady", "surprised", 55},
}};

}  // namespace

ReactionEngine::~ReactionEngine() {
    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
    }
}

bool ReactionEngine::Initialize(Callbacks callbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }
    callbacks_ = std::move(callbacks);
    esp_timer_create_args_t args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reaction_expiry",
        .skip_unhandled_events = true,
    };
    initialized_ = esp_timer_create(&args, &timer_) == ESP_OK;
    return initialized_;
}

bool ReactionEngine::ResolvePlan(const std::string& name, Plan& plan) {
    for (const auto& definition : kDefinitions) {
        if (name == definition.name) {
            plan = {
                .name = definition.name,
                .emotion = definition.emotion,
                .light_effect = definition.light_effect,
                .motion_emotion = definition.motion_emotion,
                .priority = definition.priority,
            };
            return true;
        }
    }
    return false;
}

bool ReactionEngine::IsSupported(const std::string& name) {
    Plan plan;
    return ResolvePlan(name, plan);
}

bool ReactionEngine::Start(const std::string& name, int duration_ms,
                           const std::string& oled_text, Source source) {
    Plan plan;
    if (!ResolvePlan(name, plan)) {
        return false;
    }

    const int safe_duration = std::clamp(duration_ms, 250, 30000);
    uint32_t generation = 0;
    uint32_t failed_release_generation = 0;
    bool replace_owned_motion = false;
    bool timer_started = false;
    Callbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return false;
        }
        if (active_ && source != source_ && plan.priority < plan_.priority) {
            return false;
        }
        const bool previous_active = active_;
        const uint32_t previous_generation = generation_;
        replace_owned_motion = previous_active && motion_state_ == MotionState::kStarted;
        ++generation_;
        generation = generation_;
        active_ = true;
        plan_ = plan;
        source_ = source;
        expires_at_us_ = esp_timer_get_time() + safe_duration * 1000LL;
        motion_state_ = plan.motion_emotion.empty() ? MotionState::kNotRequested
                                                    : MotionState::kPending;
        oled_active_ = !oled_text.empty();
        callbacks = callbacks_;
        esp_timer_stop(timer_);
        timer_started = esp_timer_start_once(timer_, safe_duration * 1000ULL) == ESP_OK;
        if (!timer_started) {
            active_ = false;
            expires_at_us_ = 0;
            failed_release_generation = previous_active ? previous_generation : 0;
        }
    }

    if (!timer_started) {
        if (failed_release_generation != 0 && callbacks.release) {
            callbacks.release(failed_release_generation);
        }
        return false;
    }

    if (callbacks.apply) {
        callbacks.apply(plan, oled_text, safe_duration, generation, replace_owned_motion);
    }
    return true;
}

bool ReactionEngine::Cancel() {
    uint32_t generation = 0;
    Callbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return false;
        }
        active_ = false;
        expires_at_us_ = 0;
        if (motion_state_ == MotionState::kPending || motion_state_ == MotionState::kStarted) {
            motion_state_ = MotionState::kCanceled;
        }
        generation = generation_;
        callbacks = callbacks_;
        if (timer_ != nullptr) {
            esp_timer_stop(timer_);
        }
    }
    if (callbacks.release) {
        callbacks.release(generation);
    }
    return true;
}

ReactionEngine::Status ReactionEngine::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Status status = {
        .active = active_,
        .name = plan_.name,
        .priority = plan_.priority,
        .generation = generation_,
        .remaining_ms = 0,
        .motion = motion_state_,
        .oled_active = active_ && oled_active_,
    };
    if (active_) {
        status.remaining_ms = static_cast<int>(
            std::max<int64_t>(0, (expires_at_us_ - esp_timer_get_time() + 999) / 1000));
    }
    return status;
}

void ReactionEngine::SetMotionState(uint32_t generation, MotionState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == generation_ &&
        (state != MotionState::kCompleted || motion_state_ == MotionState::kStarted)) {
        motion_state_ = state;
    }
}

void ReactionEngine::SetOledActive(uint32_t generation, bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation == generation_) {
        oled_active_ = active;
    }
}

bool ReactionEngine::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool ReactionEngine::IsActiveGeneration(uint32_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && generation == generation_;
}

const char* ReactionEngine::MotionStateName(MotionState state) {
    switch (state) {
        case MotionState::kPending:
            return "pending";
        case MotionState::kStarted:
            return "started";
        case MotionState::kSkippedDisabled:
            return "skipped_disabled";
        case MotionState::kSkippedBusy:
            return "skipped_busy";
        case MotionState::kSkippedUnsafe:
            return "skipped_unsafe";
        case MotionState::kRejected:
            return "rejected";
        case MotionState::kCompleted:
            return "completed";
        case MotionState::kCanceled:
            return "canceled";
        case MotionState::kSuperseded:
            return "superseded";
        case MotionState::kNotRequested:
        default:
            return "not_requested";
    }
}

void ReactionEngine::TimerCallback(void* arg) {
    static_cast<ReactionEngine*>(arg)->HandleTimer();
}

void ReactionEngine::HandleTimer() {
    uint32_t generation = 0;
    Callbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        // A callback already queued by an older arm must not expire a newer reaction.
        if (esp_timer_get_time() + 1000 < expires_at_us_) {
            return;
        }
        active_ = false;
        expires_at_us_ = 0;
        if (motion_state_ == MotionState::kPending || motion_state_ == MotionState::kStarted) {
            motion_state_ = MotionState::kCompleted;
        }
        generation = generation_;
        callbacks = callbacks_;
    }
    if (callbacks.release) {
        callbacks.release(generation);
    }
}
