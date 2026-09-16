#pragma once

#include <driver/gpio.h>
#include <esp_timer.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class MotorController {
public:
    static constexpr int kMinSpeedPercent = 55;
    static constexpr int kMaxSpeedPercent = 100;

    enum class Direction { kForward, kBackward, kLeft, kRight };

    struct Movement {
        Direction direction;
        uint32_t duration_ms;
        uint8_t intensity_percent = 100;
    };

    struct Status {
        bool available = false;
        bool faulted = false;
        bool moving = false;
        Direction direction = Direction::kForward;
        uint8_t intensity_percent = 0;
        size_t queued = 0;
        uint32_t remaining_ms = 0;
        bool sequence_active = false;
        size_t sequence_total = 0;
        size_t sequence_completed = 0;
    };

    MotorController(gpio_num_t left_in1, gpio_num_t left_in2, gpio_num_t right_in1,
                    gpio_num_t right_in2);
    ~MotorController();

    bool Drive(Direction direction, uint32_t duration_ms, uint8_t intensity_percent = 100);
    bool PlaySequence(const std::vector<Movement>& movements);
    void Stop();
    void EmergencyStop();
    void SetSpeedPercent(int percent);
    int GetSpeedPercent() const { return speed_percent_.load(); }
    bool IsActive() const { return motion_active_.load(std::memory_order_relaxed); }
    bool SetActiveIntensityPercent(uint8_t intensity_percent);
    void SetMotionGuard(std::function<bool(Direction)> guard);
    void SetMovementStateCallback(std::function<void(bool)> callback);
    bool IsMoving(Direction direction) const;
    Status GetStatus() const;
    std::string StatusJson() const;
    static std::string StatusJson(const Status& status);

    static const char* DirectionName(Direction direction);

private:
    enum class Phase { kIdle, kDeadTime, kDriving };

    struct Command {
        Direction direction;
        uint32_t duration_ms;
        uint8_t intensity_percent;
    };

    // Keep every movement bounded. The dead time guarantees that the bridge
    // sees LOW/LOW before either motor is driven in the opposite direction.
    static constexpr size_t kMaxQueuedCommands = 12;
    static constexpr uint32_t kMinDurationMs = 50;
    static constexpr uint32_t kMaxDurationMs = 10000;
    static constexpr uint32_t kMaxQueuedRuntimeMs = 13500;
    static constexpr size_t kMaxSequenceCommands = 50;
    static constexpr uint32_t kMaxSequenceRuntimeMs = 50000;
    static constexpr uint32_t kDirectionDeadTimeMs = 80;
    static constexpr uint32_t kStartupArmDelayMs = 1500;

    static void StopTimerCallback(void* arg);
    void BeginDeadTime();
    void ApplyNextCommand();
    void HandleTimerExpired(uint32_t generation);
    bool ArmTimer(uint32_t delay_ms);
    void EnterFault(const char* reason);
    void PublishMotionActive(bool active);
    void AllOff();
    bool InitializePwm();
    void OutputsOffLocked();
    bool SetMotor(gpio_num_t in1, gpio_num_t in2, bool forward, uint8_t intensity_percent);
    bool SetInput(gpio_num_t pin, uint32_t duty);

    gpio_num_t left_in1_;
    gpio_num_t left_in2_;
    gpio_num_t right_in1_;
    gpio_num_t right_in2_;
    esp_timer_handle_t stop_timer_ = nullptr;
    bool available_ = false;
    // Serializes timer/sensor emergency stops with application PWM writes.
    std::mutex output_mutex_;
    bool pwm_ready_ = false;
    std::atomic_int speed_percent_{100};
    std::atomic_bool emergency_pending_{false};
    std::atomic<bool> faulted_{false};
    Phase phase_ = Phase::kIdle;
    std::atomic<Direction> direction_{Direction::kForward};
    std::atomic<uint8_t> active_intensity_percent_{100};
    std::atomic<bool> moving_{false};
    std::atomic<size_t> queued_count_{0};
    std::atomic<uint32_t> queued_runtime_ms_{0};
    std::atomic<bool> sequence_active_{false};
    std::atomic<size_t> sequence_total_{0};
    std::atomic<size_t> sequence_completed_{0};
    std::atomic<int64_t> active_until_us_{0};
    std::atomic<uint32_t> timer_generation_{0};
    int64_t arm_at_us_ = 0;
    std::deque<Command> queued_commands_;
    std::function<bool(Direction)> motion_guard_;
    std::function<void(bool)> movement_state_callback_;
    std::atomic_bool motion_active_{false};
};
