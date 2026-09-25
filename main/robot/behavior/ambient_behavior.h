#pragma once

#include "ambient_behavior_types.h"
#include "reaction_engine.h"
#include "sensors/environment_types.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class AmbientBehavior {
public:
    enum class Activity : uint8_t {
        kIdle,
        kListening,
        kThinking,
        kSpeaking,
        kSuppressed,
    };

    struct Context {
        Activity activity = Activity::kSuppressed;
        bool camera_active = false;
        bool tool_active = false;
        bool manual_control_active = false;
        bool motor_busy = false;
        bool floor_safe = true;
        bool gyro_busy = false;
        bool battery_valid = false;
        int battery_percent = -1;
        bool charging = false;
        LightLevel light_level = LightLevel::kUnavailable;
        AmbientGazePersonality gaze_personality = AmbientGazePersonality::kSuppressed;
    };

    struct Callbacks {
        std::function<void(uint32_t)> reset_primitives;
        std::function<void(uint32_t, int8_t, int8_t)> set_gaze;
        std::function<void(uint32_t)> clear_gaze;
        std::function<void(uint32_t)> trigger_mouth;
        std::function<void(uint32_t)> trigger_yawn;
    };

    bool Initialize(ReactionEngine& reaction_engine, Callbacks callbacks);
    void Tick(const Context& context, int64_t now_us);
    void NotifyInteraction(int64_t now_us);

private:
    enum class IdleStage : uint8_t { kAwake, kRelaxed, kCurious, kSleepy };
    enum class GazeAction : uint8_t { kNone, kSet, kClear };
    enum class GazeProfile : uint8_t {
        kSuppressed,
        kListening,
        kThinking,
        kSpeaking,
        kHappy,
        kCool,
        kRelaxedFace,
        kSleepyFace,
        kConfused,
        kSuspicious,
        kIdleAwake,
        kIdleRelaxed,
        kIdleCurious,
        kIdleSleepy,
    };

    struct PendingActions {
        bool reset_primitives = false;
        uint32_t primitive_generation = 0;
        GazeAction gaze_action = GazeAction::kNone;
        int8_t gaze_x = 0;
        int8_t gaze_y = 0;
        bool trigger_mouth = false;
        bool trigger_yawn = false;
        uint32_t cancel_reaction_generation = 0;
        bool start_reaction = false;
        std::string reaction_name;
        int reaction_duration_ms = 0;
        uint32_t start_token = 0;
        uint32_t policy_generation = 0;
        int64_t requested_at_us = 0;
    };

    static int64_t RandomDelayUs(int minimum_ms, int maximum_ms);
    static bool IsBatteryLow(const Context& context);
    static bool IsBatteryCritical(const Context& context);
    static bool IsDim(const Context& context);
    static bool IsDark(const Context& context);

    void ExecuteActions(PendingActions actions);
    void BumpPrimitiveGenerationLocked(PendingActions& actions);
    void DetachOwnedReactionLocked(PendingActions& actions);
    bool RefreshOwnedReactionLocked(const ReactionEngine::Status& status);
    void ResetIdleSessionLocked(int64_t now_us, bool start_idle);
    static GazeProfile ResolveGazeProfile(const Context& context, IdleStage stage);
    void UpdateGazeProfileLocked(GazeProfile profile, int64_t now_us,
                                 PendingActions& actions);
    void ScheduleNextGazeLocked(const Context& context, GazeProfile profile, int64_t now_us);
    void AdvanceGazeLocked(const Context& context, GazeProfile profile, int64_t now_us,
                           PendingActions& actions);
    void AdvanceIdleActionsLocked(const Context& context, IdleStage stage, int64_t now_us,
                                  PendingActions& actions);
    IdleStage ResolveIdleStageLocked(const Context& context, int64_t now_us) const;
    bool PrepareSemanticReactionLocked(const char* name, int duration_ms, int64_t now_us,
                                       PendingActions& actions);

    std::mutex mutex_;
    ReactionEngine* reaction_engine_ = nullptr;
    Callbacks callbacks_;
    bool initialized_ = false;

    Activity activity_ = Activity::kSuppressed;
    bool hard_suppressed_ = true;
    bool idle_session_active_ = false;
    bool gaze_holding_ = false;
    GazeProfile gaze_profile_ = GazeProfile::kSuppressed;
    uint32_t primitive_generation_ = 0;
    uint32_t owned_reaction_generation_ = 0;
    std::string owned_reaction_name_;
    bool reaction_start_pending_ = false;
    uint32_t next_start_token_ = 0;
    int64_t idle_started_us_ = 0;
    int64_t next_gaze_us_ = 0;
    int64_t next_mouth_us_ = 0;
    int64_t next_semantic_us_ = 0;
    int64_t yawn_due_us_ = 0;
    int64_t last_yawn_us_ = 0;
};
