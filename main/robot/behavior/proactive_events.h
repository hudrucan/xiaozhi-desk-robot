#pragma once

#include "reaction_engine.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

class ProactiveEvents {
public:
    enum class Event : uint8_t {
        kBatteryLow,
        kBatteryCritical,
        kChargingStarted,
        kChargingFull,
        kNetworkLost,
        kNetworkRestored,
        kPickedUp,
        kPutDown,
        kCliffDetected,
        kImpact,
        kShake,
        kUpsideDown,
        kCameraStart,
        kCameraAnalyzing,
        kCameraSuccess,
        kCameraFailed,
        kUserAttention,
        kToolSuccess,
        kToolError,
        kCount,
    };

    struct BatteryObservation {
        bool valid = false;
        int percent = -1;
        bool charging = false;
        bool full_anchored = false;
    };

    struct Definition {
        const char* reaction;
        const char* oled_text;
        int duration_ms;
        int priority;
        int cooldown_ms;
        bool suppress_during_conversation;
    };

    bool Initialize(ReactionEngine& reaction_engine);
    bool Signal(Event event, int64_t now_us, bool conversation_active,
                int duration_override_ms = 0);
    void ObserveBattery(const BatteryObservation& battery, int64_t now_us,
                        bool conversation_active);
    void ObserveNetwork(bool connected, bool disconnected, int64_t now_us,
                        bool conversation_active);
    void ObserveFloor(bool available, bool floor_safe, bool floor_unsafe, bool unsafe_motion,
                      bool gyro_active, int64_t now_us, bool conversation_active);
    void Tick(int64_t now_us, bool conversation_active);

private:
    static const Definition& GetDefinition(Event event);
    bool TriggerLocked(Event event, int64_t now_us, bool conversation_active,
                       int duration_override_ms = 0);
    void RefreshOwnedReactionLocked();

    static constexpr size_t kEventCount = static_cast<size_t>(Event::kCount);
    std::mutex mutex_;
    ReactionEngine* reaction_engine_ = nullptr;
    std::array<int64_t, kEventCount> last_attempt_us_{};
    uint32_t owned_generation_ = 0;
    Event owned_event_ = Event::kCount;
    Event last_camera_event_ = Event::kCount;
    int owned_priority_ = 0;

    bool battery_seen_ = false;
    bool battery_low_latched_ = false;
    bool battery_critical_latched_ = false;
    bool charging_latched_ = false;
    bool previous_full_anchored_ = false;
    int64_t battery_low_candidate_us_ = 0;
    int64_t battery_critical_candidate_us_ = 0;
    int64_t charging_candidate_us_ = 0;

    bool network_ever_connected_ = false;
    bool network_lost_latched_ = false;
    bool network_connected_observed_ = false;
    bool network_disconnected_observed_ = false;
    int64_t network_candidate_us_ = 0;

    bool picked_up_latched_ = false;
    bool floor_loss_classified_as_cliff_ = false;
    int64_t floor_unsafe_candidate_us_ = 0;
    int64_t floor_safe_candidate_us_ = 0;

};
