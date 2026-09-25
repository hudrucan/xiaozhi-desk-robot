#include "proactive_events.h"

#include "config/tuning.h"

#include <array>

namespace {

constexpr int kPriorityAmbient = 20;
constexpr int kPriorityInfo = 40;
constexpr int kPriorityInteraction = 60;
constexpr int kPrioritySystemHigh = 80;
constexpr int kPrioritySafety = 100;

using Event = ProactiveEvents::Event;
using Definition = ProactiveEvents::Definition;

constexpr std::array<Definition, static_cast<size_t>(Event::kCount)> kDefinitions = {{
    {"warning", "Low battery", 2500, kPrioritySystemHigh,
     PROACTIVE_BATTERY_LOW_COOLDOWN_MS, false},
    {"error", "", 2500, kPrioritySafety, PROACTIVE_BATTERY_CRITICAL_COOLDOWN_MS, false},
    {"acknowledge", "Charging", 1800, kPriorityInfo, 0, true},
    {"success", "Charged", 1800, kPriorityInfo, 0, true},
    {"confused", "Offline", 2500, kPrioritySystemHigh, 0, false},
    {"success", "Online", 1800, kPriorityInfo, 0, true},
    {"startled", "", 1400, kPriorityInteraction, 0, false},
    {"acknowledge", "", 1400, kPriorityInteraction, 0, false},
    {"warning", "", 2500, kPrioritySafety, PROACTIVE_CLIFF_COOLDOWN_MS, false},
    {"startled", "", 1400, kPriorityInteraction, MPU6050_GESTURE_COOLDOWN_MS, false},
    {"nope", "", 1400, kPriorityInteraction, MPU6050_GESTURE_COOLDOWN_MS, false},
    {"sleepy", "", 1800, kPriorityInteraction, MPU6050_GESTURE_COOLDOWN_MS, false},
    {"startled", "", CAMERA_REACTION_PENDING_MS, kPriorityInteraction, 0, false},
    {"thinking", "", CAMERA_REACTION_PENDING_MS, kPriorityInteraction, 0, false},
    {"success", "", CAMERA_REACTION_SUCCESS_MS, kPriorityInteraction, 0, false},
    {"confused", "", CAMERA_REACTION_FAILURE_MS, kPrioritySystemHigh, 0, false},
    {"sleepy", "", 5000, kPriorityAmbient, PROACTIVE_IDLE_COOLDOWN_MS, true},
    {"attention", "", 1800, kPriorityInteraction, PROACTIVE_USER_ATTENTION_COOLDOWN_MS, false},
    {"success", "", 1500, kPriorityInfo, PROACTIVE_TOOL_RESULT_COOLDOWN_MS, true},
    {"error", "", 1800, kPrioritySystemHigh, PROACTIVE_TOOL_RESULT_COOLDOWN_MS, false},
}};

int64_t MillisecondsToMicroseconds(int value) {
    return static_cast<int64_t>(value) * 1000;
}

bool IsLifecycleSuccessor(Event incoming, Event current) {
    const bool incoming_camera = incoming >= Event::kCameraStart && incoming <= Event::kCameraFailed;
    const bool current_camera = current >= Event::kCameraStart && current <= Event::kCameraFailed;
    return (incoming_camera && current_camera && incoming != current) ||
           (incoming == Event::kNetworkRestored && current == Event::kNetworkLost) ||
           (incoming == Event::kPutDown && current == Event::kPickedUp) ||
           (incoming == Event::kChargingFull && current == Event::kChargingStarted);
}

}  // namespace

bool ProactiveEvents::Initialize(ReactionEngine& reaction_engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    reaction_engine_ = &reaction_engine;
    return true;
}

const ProactiveEvents::Definition& ProactiveEvents::GetDefinition(Event event) {
    return kDefinitions[static_cast<size_t>(event)];
}

void ProactiveEvents::RefreshOwnedReactionLocked() {
    if (owned_generation_ == 0 || reaction_engine_ == nullptr) {
        return;
    }
    const ReactionEngine::Status status = reaction_engine_->GetStatus();
    if (!status.active || status.generation != owned_generation_) {
        owned_generation_ = 0;
        owned_event_ = Event::kCount;
        owned_priority_ = 0;
    }
}

bool ProactiveEvents::TriggerLocked(Event event, int64_t now_us, bool conversation_active,
                                    int duration_override_ms) {
    if (reaction_engine_ == nullptr || event == Event::kCount) {
        return false;
    }
    const Definition& definition = GetDefinition(event);
    const size_t index = static_cast<size_t>(event);
    const int64_t cooldown_us = MillisecondsToMicroseconds(definition.cooldown_ms);
    if (last_attempt_us_[index] != 0 &&
        now_us - last_attempt_us_[index] < cooldown_us) {
        return false;
    }

    // Consume the edge before suppression/arbitration so a stable condition cannot retry every
    // sensor tick while conversation or a stronger reaction remains active.
    last_attempt_us_[index] = now_us;
    if (conversation_active && definition.suppress_during_conversation) {
        return false;
    }

    RefreshOwnedReactionLocked();
    if (owned_generation_ != 0) {
        if (owned_event_ == event ||
            (definition.priority <= owned_priority_ &&
             !IsLifecycleSuccessor(event, owned_event_))) {
            return false;
        }
    }

    const int duration_ms = duration_override_ms > 0 ? duration_override_ms : definition.duration_ms;
    uint32_t generation = 0;
    if (!reaction_engine_->Start(definition.reaction, duration_ms, definition.oled_text,
                                 ReactionEngine::Source::kProactive, &generation)) {
        return false;
    }
    if (reaction_engine_->IsActiveGeneration(generation)) {
        owned_generation_ = generation;
        owned_event_ = event;
        owned_priority_ = definition.priority;
    }
    return true;
}

bool ProactiveEvents::Signal(Event event, int64_t now_us, bool conversation_active,
                             int duration_override_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event >= Event::kCameraStart && event <= Event::kCameraFailed) {
        if (event == last_camera_event_) {
            return false;
        }
        last_camera_event_ = event;
    }
    if (event == Event::kCliffDetected) {
        floor_loss_classified_as_cliff_ = true;
        floor_unsafe_candidate_us_ = 0;
    }
    return TriggerLocked(event, now_us, conversation_active, duration_override_ms);
}

void ProactiveEvents::ObserveBattery(const BatteryObservation& battery, int64_t now_us,
                                     bool conversation_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!battery.valid) {
        battery_low_candidate_us_ = 0;
        battery_critical_candidate_us_ = 0;
        charging_candidate_us_ = 0;
        return;
    }

    if (!battery_seen_) {
        battery_seen_ = true;
        previous_full_anchored_ = battery.full_anchored;
    } else if (!previous_full_anchored_ && battery.full_anchored) {
        TriggerLocked(Event::kChargingFull, now_us, conversation_active);
        previous_full_anchored_ = true;
    } else if (!battery.full_anchored) {
        previous_full_anchored_ = false;
    }

    if (battery.charging) {
        battery_low_latched_ = false;
        battery_critical_latched_ = false;
        battery_low_candidate_us_ = 0;
        battery_critical_candidate_us_ = 0;
        if (!charging_latched_) {
            if (charging_candidate_us_ == 0) {
                charging_candidate_us_ = now_us;
            } else if (now_us - charging_candidate_us_ >=
                       MillisecondsToMicroseconds(PROACTIVE_CHARGING_QUALIFICATION_MS)) {
                TriggerLocked(Event::kChargingStarted, now_us, conversation_active);
                charging_latched_ = true;
            }
        }
        return;
    }

    charging_candidate_us_ = 0;
    charging_latched_ = false;
    if (battery.percent > PROACTIVE_BATTERY_LOW_REARM_PERCENT) {
        battery_low_latched_ = false;
    }
    if (battery.percent > PROACTIVE_BATTERY_CRITICAL_REARM_PERCENT) {
        battery_critical_latched_ = false;
    }

    if (!battery_critical_latched_ &&
        battery.percent <= PROACTIVE_BATTERY_CRITICAL_PERCENT) {
        if (battery_critical_candidate_us_ == 0) {
            battery_critical_candidate_us_ = now_us;
        } else if (now_us - battery_critical_candidate_us_ >=
                   MillisecondsToMicroseconds(PROACTIVE_BATTERY_CRITICAL_QUALIFICATION_MS)) {
            TriggerLocked(Event::kBatteryCritical, now_us, conversation_active);
            battery_critical_latched_ = true;
            battery_low_latched_ = true;
        }
    } else if (battery.percent > PROACTIVE_BATTERY_CRITICAL_PERCENT) {
        battery_critical_candidate_us_ = 0;
    }

    if (!battery_low_latched_ && battery.percent < PROACTIVE_BATTERY_LOW_PERCENT) {
        if (battery_low_candidate_us_ == 0) {
            battery_low_candidate_us_ = now_us;
        } else if (now_us - battery_low_candidate_us_ >=
                   MillisecondsToMicroseconds(PROACTIVE_BATTERY_LOW_QUALIFICATION_MS)) {
            TriggerLocked(Event::kBatteryLow, now_us, conversation_active);
            battery_low_latched_ = true;
        }
    } else if (battery.percent >= PROACTIVE_BATTERY_LOW_PERCENT) {
        battery_low_candidate_us_ = 0;
    }
}

void ProactiveEvents::ObserveNetwork(bool connected, bool disconnected, int64_t now_us,
                                     bool conversation_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected) {
        network_ever_connected_ = true;
        network_disconnected_observed_ = false;
        if (network_lost_latched_ && !network_connected_observed_) {
            network_connected_observed_ = true;
            network_candidate_us_ = now_us;
        }
    } else {
        // Any transition away from Connected invalidates a tentative restore. Only an explicit
        // Disconnected event may begin the initial lost debounce.
        if (network_connected_observed_) {
            network_connected_observed_ = false;
            network_candidate_us_ = 0;
        }
        if (disconnected && network_ever_connected_ && !network_lost_latched_ &&
            !network_disconnected_observed_) {
            network_disconnected_observed_ = true;
            network_candidate_us_ = now_us;
        }
    }
    (void)conversation_active;
}

void ProactiveEvents::ObserveFloor(bool available, bool floor_safe, bool floor_unsafe,
                                   bool unsafe_motion, bool gyro_active, int64_t now_us,
                                   bool conversation_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!available) {
        floor_unsafe_candidate_us_ = 0;
        floor_safe_candidate_us_ = 0;
        return;
    }
    if (floor_unsafe) {
        floor_safe_candidate_us_ = 0;
        if (unsafe_motion || gyro_active) {
            floor_loss_classified_as_cliff_ = true;
            floor_unsafe_candidate_us_ = 0;
            TriggerLocked(Event::kCliffDetected, now_us, conversation_active);
            return;
        }
        if (!picked_up_latched_ && !floor_loss_classified_as_cliff_) {
            if (floor_unsafe_candidate_us_ == 0) {
                floor_unsafe_candidate_us_ = now_us;
            } else if (now_us - floor_unsafe_candidate_us_ >=
                       MillisecondsToMicroseconds(PROACTIVE_PICKUP_QUALIFICATION_MS)) {
                TriggerLocked(Event::kPickedUp, now_us, conversation_active);
                picked_up_latched_ = true;
            }
        }
        return;
    }

    if (!floor_safe) {
        floor_unsafe_candidate_us_ = 0;
        floor_safe_candidate_us_ = 0;
        return;
    }

    floor_unsafe_candidate_us_ = 0;
    floor_loss_classified_as_cliff_ = false;
    if (!picked_up_latched_) {
        floor_safe_candidate_us_ = 0;
        return;
    }
    if (floor_safe_candidate_us_ == 0) {
        floor_safe_candidate_us_ = now_us;
    } else if (now_us - floor_safe_candidate_us_ >=
               MillisecondsToMicroseconds(PROACTIVE_PUT_DOWN_QUALIFICATION_MS)) {
        TriggerLocked(Event::kPutDown, now_us, conversation_active);
        picked_up_latched_ = false;
        floor_safe_candidate_us_ = 0;
    }
}

void ProactiveEvents::ObserveIdle(bool idle, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    idle_observed_ = idle;
    if (!idle) {
        idle_started_us_ = 0;
        idle_event_fired_ = false;
    } else if (idle_started_us_ == 0) {
        idle_started_us_ = now_us;
    }
}

void ProactiveEvents::Tick(int64_t now_us, bool conversation_active) {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshOwnedReactionLocked();

    if (network_disconnected_observed_ && network_candidate_us_ != 0 &&
        now_us - network_candidate_us_ >=
            MillisecondsToMicroseconds(PROACTIVE_NETWORK_LOST_QUALIFICATION_MS)) {
        TriggerLocked(Event::kNetworkLost, now_us, conversation_active);
        network_lost_latched_ = true;
        network_disconnected_observed_ = false;
        network_candidate_us_ = 0;
    } else if (network_connected_observed_ && network_lost_latched_ &&
               network_candidate_us_ != 0 &&
               now_us - network_candidate_us_ >=
                   MillisecondsToMicroseconds(PROACTIVE_NETWORK_RESTORED_QUALIFICATION_MS)) {
        TriggerLocked(Event::kNetworkRestored, now_us, conversation_active);
        network_lost_latched_ = false;
        network_connected_observed_ = false;
        network_candidate_us_ = 0;
    }

    if (idle_observed_ && !idle_event_fired_ && idle_started_us_ != 0 &&
        now_us - idle_started_us_ >= MillisecondsToMicroseconds(PROACTIVE_IDLE_LONG_MS)) {
        TriggerLocked(Event::kIdleLong, now_us, conversation_active);
        idle_event_fired_ = true;
    }
}
