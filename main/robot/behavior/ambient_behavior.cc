#include "ambient_behavior.h"

#include "config/tuning.h"

#include <esp_random.h>

#include <algorithm>
#include <utility>

namespace {

constexpr int64_t kUsPerMs = 1000;

}  // namespace

bool AmbientBehavior::Initialize(ReactionEngine& reaction_engine, Callbacks callbacks) {
    PendingActions actions;
    bool initialized = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) {
            return true;
        }
        reaction_engine_ = &reaction_engine;
        callbacks_ = std::move(callbacks);
        initialized_ = callbacks_.reset_primitives && callbacks_.set_gaze &&
                       callbacks_.clear_gaze && callbacks_.trigger_mouth &&
                       callbacks_.trigger_yawn;
        initialized = initialized_;
        if (initialized_) {
            BumpPrimitiveGenerationLocked(actions);
        }
    }
    ExecuteActions(std::move(actions));
    return initialized;
}

int64_t AmbientBehavior::RandomDelayUs(int minimum_ms, int maximum_ms) {
    const int span = std::max(1, maximum_ms - minimum_ms + 1);
    return static_cast<int64_t>(minimum_ms + esp_random() % span) * kUsPerMs;
}

bool AmbientBehavior::IsBatteryLow(const Context& context) {
    return context.battery_valid && !context.charging &&
           context.battery_percent <= PROACTIVE_BATTERY_LOW_PERCENT;
}

bool AmbientBehavior::IsBatteryCritical(const Context& context) {
    return context.battery_valid && !context.charging &&
           context.battery_percent <= PROACTIVE_BATTERY_CRITICAL_PERCENT;
}

bool AmbientBehavior::IsDim(const Context& context) {
    return context.light_level == LightLevel::kDark ||
           context.light_level == LightLevel::kDim;
}

bool AmbientBehavior::IsDark(const Context& context) {
    return context.light_level == LightLevel::kDark;
}

void AmbientBehavior::ExecuteActions(PendingActions actions) {
    if (actions.reset_primitives) {
        callbacks_.reset_primitives(actions.primitive_generation);
    }
    if (actions.gaze_action == GazeAction::kSet) {
        callbacks_.set_gaze(actions.primitive_generation, actions.gaze_x, actions.gaze_y);
    } else if (actions.gaze_action == GazeAction::kClear) {
        callbacks_.clear_gaze(actions.primitive_generation);
    }
    if (actions.trigger_mouth) {
        callbacks_.trigger_mouth(actions.primitive_generation);
    }
    if (actions.trigger_yawn) {
        callbacks_.trigger_yawn(actions.primitive_generation);
    }
    if (actions.cancel_reaction_generation != 0 && reaction_engine_ != nullptr) {
        reaction_engine_->CancelIfGeneration(actions.cancel_reaction_generation);
    }
    if (!actions.start_reaction || reaction_engine_ == nullptr) {
        return;
    }

    uint32_t generation = 0;
    const bool started = reaction_engine_->Start(
        actions.reaction_name, actions.reaction_duration_ms, "",
        ReactionEngine::Source::kAmbient, &generation);
    const bool active = started && generation != 0 &&
                        reaction_engine_->IsActiveGeneration(generation);
    bool cancel_stale_generation = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool intent_current = reaction_start_pending_ &&
                                    actions.start_token == next_start_token_ &&
                                    actions.policy_generation == primitive_generation_;
        if (intent_current) {
            reaction_start_pending_ = false;
            if (active && activity_ == Activity::kIdle && !hard_suppressed_ &&
                owned_reaction_generation_ == 0) {
                owned_reaction_generation_ = generation;
                owned_reaction_name_ = actions.reaction_name;
                if (owned_reaction_name_ == "sleepy" &&
                    (last_yawn_us_ == 0 ||
                     actions.requested_at_us - last_yawn_us_ >=
                         AMBIENT_YAWN_COOLDOWN_MS * kUsPerMs)) {
                    yawn_due_us_ = actions.requested_at_us +
                                   AMBIENT_YAWN_REACTION_DELAY_MS * kUsPerMs;
                }
                next_semantic_us_ = actions.requested_at_us +
                                    RandomDelayUs(AMBIENT_SEMANTIC_INTERVAL_MIN_MS,
                                                  AMBIENT_SEMANTIC_INTERVAL_MAX_MS);
            } else {
                next_semantic_us_ = actions.requested_at_us +
                                    RandomDelayUs(AMBIENT_SEMANTIC_RETRY_MIN_MS,
                                                  AMBIENT_SEMANTIC_RETRY_MAX_MS);
                cancel_stale_generation = started && generation != 0;
            }
        } else {
            cancel_stale_generation = started && generation != 0;
        }
    }
    if (cancel_stale_generation) {
        reaction_engine_->CancelIfGeneration(generation);
    }
}

void AmbientBehavior::BumpPrimitiveGenerationLocked(PendingActions& actions) {
    ++primitive_generation_;
    if (primitive_generation_ == 0) {
        ++primitive_generation_;
    }
    reaction_start_pending_ = false;
    gaze_holding_ = false;
    next_gaze_us_ = 0;
    gaze_profile_ = GazeProfile::kSuppressed;
    actions.reset_primitives = true;
    actions.primitive_generation = primitive_generation_;
    actions.gaze_action = GazeAction::kNone;
    actions.gaze_x = 0;
    actions.gaze_y = 0;
    actions.trigger_mouth = false;
    actions.trigger_yawn = false;
}

void AmbientBehavior::DetachOwnedReactionLocked(PendingActions& actions) {
    if (owned_reaction_generation_ == 0) {
        return;
    }
    actions.cancel_reaction_generation = owned_reaction_generation_;
    owned_reaction_generation_ = 0;
    owned_reaction_name_.clear();
    yawn_due_us_ = 0;
}

bool AmbientBehavior::RefreshOwnedReactionLocked(const ReactionEngine::Status& status) {
    if (owned_reaction_generation_ == 0 ||
        (status.active && status.generation == owned_reaction_generation_)) {
        return false;
    }
    owned_reaction_generation_ = 0;
    owned_reaction_name_.clear();
    yawn_due_us_ = 0;
    return true;
}

void AmbientBehavior::ResetIdleSessionLocked(int64_t now_us, bool start_idle) {
    idle_session_active_ = start_idle;
    idle_started_us_ = start_idle ? now_us : 0;
    next_mouth_us_ = start_idle
                         ? now_us + AMBIENT_IDLE_MOUTH_START_MS * kUsPerMs +
                               RandomDelayUs(AMBIENT_MOUTH_INTERVAL_MIN_MS,
                                             AMBIENT_MOUTH_INTERVAL_MAX_MS)
                         : 0;
    next_semantic_us_ = start_idle
                            ? now_us + AMBIENT_CURIOUS_START_MS * kUsPerMs +
                                  RandomDelayUs(AMBIENT_SEMANTIC_JITTER_MIN_MS,
                                                AMBIENT_SEMANTIC_JITTER_MAX_MS)
                            : 0;
    yawn_due_us_ = 0;
}

AmbientBehavior::GazeProfile AmbientBehavior::ResolveGazeProfile(
    const Context& context, IdleStage stage) {
    if (context.gaze_personality == AmbientGazePersonality::kSuppressed) {
        return GazeProfile::kSuppressed;
    }
    if (context.activity == Activity::kListening) {
        return GazeProfile::kListening;
    }
    if (context.activity == Activity::kThinking) {
        return GazeProfile::kThinking;
    }
    if (context.activity == Activity::kSpeaking) {
        return GazeProfile::kSpeaking;
    }
    switch (context.gaze_personality) {
        case AmbientGazePersonality::kListening:
            return GazeProfile::kListening;
        case AmbientGazePersonality::kThinking:
            return GazeProfile::kThinking;
        case AmbientGazePersonality::kSpeaking:
            return GazeProfile::kSpeaking;
        case AmbientGazePersonality::kHappy:
            return GazeProfile::kHappy;
        case AmbientGazePersonality::kCool:
            return GazeProfile::kCool;
        case AmbientGazePersonality::kRelaxed:
            return GazeProfile::kRelaxedFace;
        case AmbientGazePersonality::kSleepy:
            return GazeProfile::kSleepyFace;
        case AmbientGazePersonality::kConfused:
            return GazeProfile::kConfused;
        case AmbientGazePersonality::kSuspicious:
            return GazeProfile::kSuspicious;
        case AmbientGazePersonality::kNeutral:
            break;
        case AmbientGazePersonality::kSuppressed:
            return GazeProfile::kSuppressed;
    }
    switch (stage) {
        case IdleStage::kAwake:
            return GazeProfile::kIdleAwake;
        case IdleStage::kRelaxed:
            return GazeProfile::kIdleRelaxed;
        case IdleStage::kCurious:
            return GazeProfile::kIdleCurious;
        case IdleStage::kSleepy:
            return GazeProfile::kIdleSleepy;
    }
    return GazeProfile::kSuppressed;
}

void AmbientBehavior::UpdateGazeProfileLocked(GazeProfile profile, int64_t now_us,
                                              PendingActions& actions) {
    if (profile == gaze_profile_) {
        return;
    }
    gaze_profile_ = profile;
    gaze_holding_ = false;
    next_gaze_us_ = profile == GazeProfile::kSuppressed
                        ? 0
                        : now_us + RandomDelayUs(500, 1800);
    actions.gaze_action = GazeAction::kClear;
    actions.primitive_generation = primitive_generation_;
}

void AmbientBehavior::ScheduleNextGazeLocked(const Context& context, GazeProfile profile,
                                             int64_t now_us) {
    int minimum_ms = AMBIENT_GAZE_INTERVAL_MIN_MS;
    int maximum_ms = AMBIENT_GAZE_INTERVAL_MAX_MS;
    switch (profile) {
        case GazeProfile::kIdleAwake:
            minimum_ms = AMBIENT_AWAKE_GAZE_INTERVAL_MIN_MS;
            maximum_ms = AMBIENT_AWAKE_GAZE_INTERVAL_MAX_MS;
            break;
        case GazeProfile::kListening:
        case GazeProfile::kSpeaking:
            minimum_ms = 7000;
            maximum_ms = 14000;
            break;
        case GazeProfile::kThinking:
            minimum_ms = 5000;
            maximum_ms = 8000;
            break;
        case GazeProfile::kHappy:
            minimum_ms = 3000;
            maximum_ms = 5200;
            break;
        case GazeProfile::kCool:
        case GazeProfile::kRelaxedFace:
        case GazeProfile::kConfused:
            minimum_ms = 5000;
            maximum_ms = 8000;
            break;
        case GazeProfile::kSleepyFace:
            minimum_ms = 6500;
            maximum_ms = 10000;
            break;
        case GazeProfile::kSuspicious:
            minimum_ms = 4500;
            maximum_ms = 7000;
            break;
        case GazeProfile::kIdleRelaxed:
        case GazeProfile::kIdleCurious:
        case GazeProfile::kIdleSleepy:
            break;
        case GazeProfile::kSuppressed:
            return;
    }
    if (IsBatteryLow(context) || IsDim(context)) {
        minimum_ms += AMBIENT_GAZE_INTERVAL_MIN_MS / 2;
        maximum_ms += AMBIENT_GAZE_INTERVAL_MAX_MS / 2;
    }
    next_gaze_us_ = now_us + RandomDelayUs(minimum_ms, maximum_ms);
}

void AmbientBehavior::AdvanceGazeLocked(const Context& context, GazeProfile profile,
                                        int64_t now_us, PendingActions& actions) {
    if (profile == GazeProfile::kSuppressed || now_us < next_gaze_us_) {
        return;
    }
    if (gaze_holding_) {
        actions.gaze_action = GazeAction::kClear;
        actions.primitive_generation = primitive_generation_;
        gaze_holding_ = false;
        ScheduleNextGazeLocked(context, profile, now_us);
        return;
    }

    const int direction = (esp_random() & 1U) == 0 ? -1 : 1;
    int x = 0;
    int y = 0;
    int hold_min_ms = AMBIENT_GAZE_HOLD_MIN_MS;
    int hold_max_ms = AMBIENT_GAZE_HOLD_MAX_MS;
    switch (profile) {
        case GazeProfile::kListening:
            x = direction;
            hold_min_ms = 500;
            hold_max_ms = 900;
            break;
        case GazeProfile::kThinking:
            x = direction * 5;
            y = -3;
            hold_min_ms = 1800;
            hold_max_ms = 3000;
            break;
        case GazeProfile::kSpeaking:
            x = direction;
            hold_min_ms = 400;
            hold_max_ms = 800;
            break;
        case GazeProfile::kHappy:
            x = direction * 6;
            y = direction < 0 ? 2 : 3;
            hold_min_ms = 1000;
            hold_max_ms = 1600;
            break;
        case GazeProfile::kCool:
            x = direction * 4;
            y = 2;
            hold_min_ms = 1600;
            hold_max_ms = 2400;
            break;
        case GazeProfile::kRelaxedFace:
            x = direction * 4;
            y = direction < 0 ? 2 : 3;
            hold_min_ms = 1700;
            hold_max_ms = 2600;
            break;
        case GazeProfile::kSleepyFace:
            x = direction * 2;
            y = 3;
            hold_min_ms = 1900;
            hold_max_ms = 2800;
            break;
        case GazeProfile::kConfused:
            x = direction < 0 ? -6 : 4;
            y = direction < 0 ? -1 : 3;
            hold_min_ms = 2000;
            hold_max_ms = 3000;
            break;
        case GazeProfile::kSuspicious:
            x = direction * 8;
            y = direction < 0 ? 1 : 3;
            hold_min_ms = 2100;
            hold_max_ms = 3100;
            break;
        case GazeProfile::kIdleAwake:
            x = direction * 8;
            y = direction < 0 ? 3 : 4;
            hold_min_ms = AMBIENT_AWAKE_GAZE_HOLD_MIN_MS;
            hold_max_ms = AMBIENT_AWAKE_GAZE_HOLD_MAX_MS;
            break;
        case GazeProfile::kIdleRelaxed:
            x = direction * 6;
            y = 3;
            break;
        case GazeProfile::kIdleCurious:
            x = direction * 8;
            y = direction < 0 ? 2 : 4;
            hold_min_ms += 300;
            hold_max_ms += 600;
            break;
        case GazeProfile::kIdleSleepy:
            x = direction * 3;
            y = 2;
            hold_min_ms += 500;
            hold_max_ms += 900;
            break;
        case GazeProfile::kSuppressed:
            return;
    }
    actions.gaze_action = GazeAction::kSet;
    actions.primitive_generation = primitive_generation_;
    actions.gaze_x = static_cast<int8_t>(x);
    actions.gaze_y = static_cast<int8_t>(y);
    gaze_holding_ = true;
    next_gaze_us_ = now_us + RandomDelayUs(hold_min_ms, hold_max_ms);
}

AmbientBehavior::IdleStage AmbientBehavior::ResolveIdleStageLocked(
    const Context& context, int64_t now_us) const {
    const int64_t idle_ms = (now_us - idle_started_us_) / kUsPerMs;
    if (idle_ms < AMBIENT_RELAXED_START_MS) {
        return IdleStage::kAwake;
    }
    if (idle_ms < AMBIENT_CURIOUS_START_MS) {
        return IdleStage::kRelaxed;
    }
    const int sleepy_start_ms = IsDark(context) ? AMBIENT_DARK_SLEEPY_START_MS
                                                 : AMBIENT_SLEEPY_START_MS;
    return idle_ms < sleepy_start_ms ? IdleStage::kCurious : IdleStage::kSleepy;
}

bool AmbientBehavior::PrepareSemanticReactionLocked(const char* name, int duration_ms,
                                                     int64_t now_us,
                                                     PendingActions& actions) {
    if (reaction_engine_ == nullptr || owned_reaction_generation_ != 0 ||
        reaction_start_pending_) {
        return false;
    }
    BumpPrimitiveGenerationLocked(actions);
    reaction_start_pending_ = true;
    ++next_start_token_;
    if (next_start_token_ == 0) {
        ++next_start_token_;
    }
    actions.start_reaction = true;
    actions.reaction_name = name;
    actions.reaction_duration_ms = duration_ms;
    actions.start_token = next_start_token_;
    actions.policy_generation = primitive_generation_;
    actions.requested_at_us = now_us;
    next_semantic_us_ = now_us +
                        RandomDelayUs(AMBIENT_SEMANTIC_RETRY_MIN_MS,
                                      AMBIENT_SEMANTIC_RETRY_MAX_MS);
    return true;
}

void AmbientBehavior::AdvanceIdleActionsLocked(const Context& context, IdleStage stage,
                                               int64_t now_us, PendingActions& actions) {
    if (owned_reaction_generation_ != 0) {
        if (owned_reaction_name_ == "sleepy" && yawn_due_us_ != 0 &&
            now_us >= yawn_due_us_) {
            actions.trigger_yawn = true;
            actions.primitive_generation = primitive_generation_;
            last_yawn_us_ = now_us;
            yawn_due_us_ = 0;
        }
        return;
    }
    if (reaction_start_pending_) {
        return;
    }

    const bool low_battery = IsBatteryLow(context);
    if (!low_battery && stage != IdleStage::kAwake && next_mouth_us_ != 0 &&
        now_us >= next_mouth_us_) {
        actions.trigger_mouth = true;
        actions.primitive_generation = primitive_generation_;
        int minimum_ms = AMBIENT_MOUTH_INTERVAL_MIN_MS;
        int maximum_ms = AMBIENT_MOUTH_INTERVAL_MAX_MS;
        if (IsDim(context)) {
            minimum_ms += AMBIENT_MOUTH_INTERVAL_MIN_MS / 2;
            maximum_ms += AMBIENT_MOUTH_INTERVAL_MAX_MS / 2;
        }
        next_mouth_us_ = now_us + RandomDelayUs(minimum_ms, maximum_ms);
    }

    if (next_semantic_us_ == 0 || now_us < next_semantic_us_) {
        return;
    }
    if (stage == IdleStage::kSleepy) {
        PrepareSemanticReactionLocked("sleepy", AMBIENT_SLEEPY_REACTION_MS, now_us, actions);
    } else if (stage == IdleStage::kCurious && !low_battery && !context.charging &&
               !IsDim(context)) {
        PrepareSemanticReactionLocked("curious", AMBIENT_CURIOUS_REACTION_MS, now_us, actions);
    } else {
        next_semantic_us_ = now_us +
                            RandomDelayUs(AMBIENT_SEMANTIC_RETRY_MIN_MS,
                                          AMBIENT_SEMANTIC_RETRY_MAX_MS);
    }
}

void AmbientBehavior::Tick(const Context& context, int64_t now_us) {
    ReactionEngine::Status reaction;
    if (reaction_engine_ != nullptr) {
        reaction = reaction_engine_->GetStatus();
    }

    PendingActions actions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return;
        }

        const bool ownership_ended = RefreshOwnedReactionLocked(reaction);
        const bool foreign_reaction =
            reaction.active && reaction.generation != owned_reaction_generation_;
        const bool owned_reaction_active = owned_reaction_generation_ != 0;
        const bool hard_suppressed = context.camera_active || context.tool_active ||
                                     context.manual_control_active ||
                                     (context.motor_busy && !owned_reaction_active) ||
                                     !context.floor_safe ||
                                     (context.gyro_busy && !owned_reaction_active) ||
                                     IsBatteryCritical(context) || foreign_reaction;
        const bool activity_changed = context.activity != activity_;
        const bool suppression_changed = hard_suppressed != hard_suppressed_;

        if (activity_changed || suppression_changed) {
            BumpPrimitiveGenerationLocked(actions);
            if (context.activity != Activity::kIdle || hard_suppressed) {
                DetachOwnedReactionLocked(actions);
            }
            activity_ = context.activity;
            hard_suppressed_ = hard_suppressed;
            ResetIdleSessionLocked(now_us,
                                   context.activity == Activity::kIdle && !hard_suppressed);
        } else if (ownership_ended) {
            BumpPrimitiveGenerationLocked(actions);
            next_mouth_us_ = std::max(next_mouth_us_, now_us + 2000 * kUsPerMs);
            next_semantic_us_ = now_us +
                                RandomDelayUs(AMBIENT_SEMANTIC_INTERVAL_MIN_MS,
                                              AMBIENT_SEMANTIC_INTERVAL_MAX_MS);
        }

        if (!hard_suppressed && context.activity != Activity::kSuppressed) {
            IdleStage stage = IdleStage::kAwake;
            if (context.activity == Activity::kIdle) {
                if (!idle_session_active_) {
                    ResetIdleSessionLocked(now_us, true);
                }
                stage = ResolveIdleStageLocked(context, now_us);
            }
            const GazeProfile gaze_profile = ResolveGazeProfile(context, stage);
            UpdateGazeProfileLocked(gaze_profile, now_us, actions);
            AdvanceGazeLocked(context, gaze_profile, now_us, actions);
            if (context.activity == Activity::kIdle) {
                AdvanceIdleActionsLocked(context, stage, now_us, actions);
            }
        }
    }
    ExecuteActions(std::move(actions));
}

void AmbientBehavior::NotifyInteraction(int64_t now_us) {
    PendingActions actions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            return;
        }
        BumpPrimitiveGenerationLocked(actions);
        DetachOwnedReactionLocked(actions);
        ResetIdleSessionLocked(now_us, activity_ == Activity::kIdle && !hard_suppressed_);
    }
    ExecuteActions(std::move(actions));
}
