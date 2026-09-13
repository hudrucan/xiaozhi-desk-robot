#include "motor_controller.h"

#include "application.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#define TAG "MotorController"

namespace {
// Desk robot: camera/backlight use timer 0, status light uses timer 2.
constexpr auto kPwmMode = LEDC_LOW_SPEED_MODE;
constexpr auto kPwmTimer = LEDC_TIMER_3;
constexpr ledc_channel_t kChannels[] = {LEDC_CHANNEL_4, LEDC_CHANNEL_5, LEDC_CHANNEL_6,
                                        LEDC_CHANNEL_7};
constexpr uint32_t kMaxDuty = 1023;
}  // namespace

MotorController::MotorController(gpio_num_t left_in1, gpio_num_t left_in2, gpio_num_t right_in1,
                                 gpio_num_t right_in2)
    : left_in1_(left_in1), left_in2_(left_in2), right_in1_(right_in1), right_in2_(right_in2) {
    const bool pins_are_outputs =
        GPIO_IS_VALID_OUTPUT_GPIO(left_in1_) && GPIO_IS_VALID_OUTPUT_GPIO(left_in2_) &&
        GPIO_IS_VALID_OUTPUT_GPIO(right_in1_) && GPIO_IS_VALID_OUTPUT_GPIO(right_in2_);
    const bool pins_are_unique = left_in1_ != left_in2_ && left_in1_ != right_in1_ &&
                                 left_in1_ != right_in2_ && left_in2_ != right_in1_ &&
                                 left_in2_ != right_in2_ && right_in1_ != right_in2_;
    available_ = pins_are_outputs && pins_are_unique;
    if (!available_) {
        ESP_LOGE(TAG, "Motor control disabled: pins must be four unique output-capable GPIOs");
        return;
    }

    // Preload the output latches LOW before enabling the pins as outputs. This
    // minimizes startup glitches that could otherwise briefly energize a motor.
    gpio_set_level(left_in1_, 0);
    gpio_set_level(left_in2_, 0);
    gpio_set_level(right_in1_, 0);
    gpio_set_level(right_in2_, 0);
    const uint64_t pins =
        (1ULL << left_in1_) | (1ULL << left_in2_) | (1ULL << right_in1_) | (1ULL << right_in2_);
    gpio_config_t config = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t gpio_result = gpio_config(&config);
    if (gpio_result != ESP_OK) {
        ESP_LOGE(TAG, "Motor GPIO setup failed: %s", esp_err_to_name(gpio_result));
        available_ = false;
        return;
    }
    AllOff();

    const esp_timer_create_args_t timer_args = {
        .callback = &MotorController::StopTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "motor_stop",
        .skip_unhandled_events = false,
    };
    const esp_err_t timer_result = esp_timer_create(&timer_args, &stop_timer_);
    if (timer_result != ESP_OK) {
        ESP_LOGE(TAG, "Motor fail-safe timer setup failed: %s", esp_err_to_name(timer_result));
        AllOff();
        available_ = false;
        return;
    }
    arm_at_us_ = esp_timer_get_time() + static_cast<int64_t>(kStartupArmDelayMs) * 1000;
    ESP_LOGI(TAG, "Motor control ready in %lu ms; max run %lu ms, dead time %lu ms",
             static_cast<unsigned long>(kStartupArmDelayMs),
             static_cast<unsigned long>(kMaxDurationMs),
             static_cast<unsigned long>(kDirectionDeadTimeMs));
}

MotorController::~MotorController() {
    Stop();
    if (stop_timer_ != nullptr) {
        esp_timer_delete(stop_timer_);
    }
}

void MotorController::SetSpeedPercent(int percent) {
    speed_percent_.store(std::clamp(percent, kMinSpeedPercent, kMaxSpeedPercent));
}

bool MotorController::InitializePwm() {
    // Called with output_mutex_ held, only when an actual movement starts.
    if (pwm_ready_) {
        return true;
    }
    ledc_timer_config_t timer = {};
    timer.speed_mode = kPwmMode;
    timer.timer_num = kPwmTimer;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.freq_hz = 20000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t result = ledc_timer_config(&timer);
    const gpio_num_t pins[] = {left_in1_, left_in2_, right_in1_, right_in2_};
    size_t configured = 0;
    while (result == ESP_OK && configured < 4) {
        ledc_channel_config_t channel = {};
        channel.gpio_num = pins[configured];
        channel.speed_mode = kPwmMode;
        channel.channel = kChannels[configured];
        channel.timer_sel = kPwmTimer;
        channel.duty = 0;
        result = ledc_channel_config(&channel);
        if (result == ESP_OK) {
            ++configured;
        }
    }
    if (result != ESP_OK) {
        for (size_t i = 0; i < configured; ++i) {
            ledc_stop(kPwmMode, kChannels[i], 0);
        }
        // Detach any partially configured channels and restore the safe GPIO state.
        for (const auto pin : pins) {
            gpio_reset_pin(pin);
            gpio_set_level(pin, 0);
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        }
        ESP_LOGE(TAG, "Motor PWM initialization failed: %s", esp_err_to_name(result));
        return false;
    }
    pwm_ready_ = true;
    ESP_LOGI(TAG, "Motor PWM ready: 20 kHz, timer 3, channels 4-7");
    return true;
}

bool MotorController::SetInput(gpio_num_t pin, uint32_t duty) {
    const gpio_num_t pins[] = {left_in1_, left_in2_, right_in1_, right_in2_};
    for (size_t i = 0; i < 4; ++i) {
        if (pins[i] == pin) {
            // Basic duty operations are serialized by output_mutex_; no fade service needed.
            return ledc_set_duty(kPwmMode, kChannels[i], duty) == ESP_OK &&
                   ledc_update_duty(kPwmMode, kChannels[i]) == ESP_OK;
        }
    }
    return false;
}

bool MotorController::SetMotor(gpio_num_t in1, gpio_num_t in2, bool forward,
                               uint8_t intensity_percent) {
    const uint32_t bounded_intensity = std::min<uint32_t>(intensity_percent, 100);
    const uint32_t duty =
        kMaxDuty * static_cast<uint32_t>(speed_percent_.load()) * bounded_intensity / 10000;
    return SetInput(forward ? in2 : in1, 0) && SetInput(forward ? in1 : in2, duty);
}

void MotorController::AllOff() {
    std::lock_guard<std::mutex> lock(output_mutex_);
    OutputsOffLocked();
}

void MotorController::OutputsOffLocked() {
    if (!available_) {
        return;
    }
    if (pwm_ready_) {
        // Keep LEDC channels configured between movements. Stopping the channels
        // while pwm_ready_ remains true would make the next movement skip re-init.
        for (const auto channel : kChannels) {
            ledc_set_duty(kPwmMode, channel, 0);
            ledc_update_duty(kPwmMode, channel);
        }
        return;
    }
    gpio_set_level(left_in1_, 0);
    gpio_set_level(left_in2_, 0);
    gpio_set_level(right_in1_, 0);
    gpio_set_level(right_in2_, 0);
}

bool MotorController::Drive(Direction direction, uint32_t duration_ms, uint8_t intensity_percent) {
    if (!available_ || faulted_.load(std::memory_order_relaxed)) {
        return false;
    }
    if (esp_timer_get_time() < arm_at_us_) {
        ESP_LOGW(TAG, "Ignoring motor command during startup arm delay");
        return false;
    }
    if (motion_guard_ && !motion_guard_(direction)) {
        ESP_LOGW(TAG, "Motor command blocked by safety guard");
        return false;
    }
    duration_ms = std::clamp(duration_ms, kMinDurationMs, kMaxDurationMs);
    if (queued_commands_.size() >= kMaxQueuedCommands) {
        ESP_LOGW(TAG, "Motor command queue is full; dropping command");
        return false;
    }
    if (queued_runtime_ms_.load(std::memory_order_relaxed) + duration_ms > kMaxQueuedRuntimeMs) {
        ESP_LOGW(TAG, "Motor command queue runtime limit reached; dropping command");
        return false;
    }

    queued_commands_.push_back(
        {direction, duration_ms, static_cast<uint8_t>(std::min<int>(intensity_percent, 100))});
    queued_runtime_ms_.fetch_add(duration_ms, std::memory_order_relaxed);
    queued_count_.store(queued_commands_.size(), std::memory_order_relaxed);
    if (queued_commands_.size() == 1 && phase_ == Phase::kIdle) {
        sequence_active_.store(false, std::memory_order_relaxed);
        sequence_total_.store(0, std::memory_order_relaxed);
        sequence_completed_.store(0, std::memory_order_relaxed);
    }
    PublishMotionActive(true);
    if (phase_ == Phase::kIdle) {
        BeginDeadTime();
    }
    return true;
}

bool MotorController::PlaySequence(const std::vector<Movement>& movements) {
    if (!available_ || faulted_.load(std::memory_order_relaxed) || movements.empty() ||
        movements.size() > kMaxSequenceCommands || esp_timer_get_time() < arm_at_us_) {
        return false;
    }

    uint32_t total_runtime_ms = 0;
    for (const auto& movement : movements) {
        if (motion_guard_ && !motion_guard_(movement.direction)) {
            ESP_LOGW(TAG, "Motor sequence blocked by safety guard");
            return false;
        }
        const uint32_t duration_ms =
            std::clamp(movement.duration_ms, kMinDurationMs, kMaxDurationMs);
        if (duration_ms > kMaxSequenceRuntimeMs - total_runtime_ms) {
            ESP_LOGW(TAG, "Motor sequence runtime limit reached");
            return false;
        }
        total_runtime_ms += duration_ms;
    }

    Stop();
    for (const auto& movement : movements) {
        queued_commands_.push_back(
            {movement.direction, std::clamp(movement.duration_ms, kMinDurationMs, kMaxDurationMs),
             static_cast<uint8_t>(std::min<int>(movement.intensity_percent, 100))});
    }
    queued_runtime_ms_.store(total_runtime_ms, std::memory_order_relaxed);
    queued_count_.store(queued_commands_.size(), std::memory_order_relaxed);
    sequence_total_.store(queued_commands_.size(), std::memory_order_relaxed);
    sequence_completed_.store(0, std::memory_order_relaxed);
    sequence_active_.store(true, std::memory_order_relaxed);
    PublishMotionActive(true);
    BeginDeadTime();
    return true;
}

bool MotorController::ArmTimer(uint32_t delay_ms) {
    if (stop_timer_ == nullptr) {
        EnterFault("fail-safe timer unavailable");
        return false;
    }
    esp_timer_stop(stop_timer_);
    timer_generation_.fetch_add(1, std::memory_order_relaxed);
    const esp_err_t result = esp_timer_start_once(stop_timer_, delay_ms * 1000ULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Motor fail-safe timer start failed: %s", esp_err_to_name(result));
        EnterFault("fail-safe timer start failed");
        return false;
    }
    return true;
}

void MotorController::BeginDeadTime() {
    if (!available_ || faulted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (queued_commands_.empty()) {
        AllOff();
        phase_ = Phase::kIdle;
        moving_.store(false, std::memory_order_relaxed);
        sequence_active_.store(false, std::memory_order_relaxed);
        active_until_us_.store(0, std::memory_order_relaxed);
        PublishMotionActive(false);
        return;
    }
    AllOff();
    moving_.store(false, std::memory_order_relaxed);
    phase_ = Phase::kDeadTime;
    ArmTimer(kDirectionDeadTimeMs);
}

void MotorController::ApplyNextCommand() {
    if (!available_ || faulted_.load(std::memory_order_relaxed) || queued_commands_.empty()) {
        AllOff();
        phase_ = Phase::kIdle;
        moving_.store(false, std::memory_order_relaxed);
        return;
    }
    const auto command = queued_commands_.front();
    queued_commands_.pop_front();
    queued_runtime_ms_.fetch_sub(command.duration_ms, std::memory_order_relaxed);
    queued_count_.store(queued_commands_.size(), std::memory_order_relaxed);
    if (motion_guard_ && !motion_guard_(command.direction)) {
        ESP_LOGW(TAG, "Queued motor command blocked by safety guard; cancelling sequence");
        Stop();
        return;
    }
    direction_.store(command.direction, std::memory_order_relaxed);
    active_intensity_percent_.store(command.intensity_percent, std::memory_order_relaxed);
    phase_ = Phase::kDriving;
    moving_.store(true, std::memory_order_relaxed);
    active_until_us_.store(esp_timer_get_time() + static_cast<int64_t>(command.duration_ms) * 1000,
                           std::memory_order_relaxed);

    bool applied = false;
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (!emergency_pending_.load() && InitializePwm()) {
            switch (command.direction) {
                case Direction::kForward:
                    applied = SetMotor(left_in1_, left_in2_, false, command.intensity_percent) &&
                              SetMotor(right_in1_, right_in2_, false, command.intensity_percent);
                    break;
                case Direction::kBackward:
                    applied = SetMotor(left_in1_, left_in2_, true, command.intensity_percent) &&
                              SetMotor(right_in1_, right_in2_, true, command.intensity_percent);
                    break;
                case Direction::kLeft:
                    applied = SetMotor(left_in1_, left_in2_, true, command.intensity_percent) &&
                              SetMotor(right_in1_, right_in2_, false, command.intensity_percent);
                    break;
                case Direction::kRight:
                    applied = SetMotor(left_in1_, left_in2_, false, command.intensity_percent) &&
                              SetMotor(right_in1_, right_in2_, true, command.intensity_percent);
                    break;
            }
        }
        if (!applied) {
            OutputsOffLocked();
        }
    }
    if (!applied) {
        if (emergency_pending_.load()) {
            Stop();
            return;
        }
        EnterFault("motor PWM unavailable; outputs disabled");
        return;
    }

    ArmTimer(command.duration_ms);
}

bool MotorController::SetActiveIntensityPercent(uint8_t intensity_percent) {
    if (!available_ || faulted_.load(std::memory_order_relaxed) ||
        !moving_.load(std::memory_order_relaxed)) {
        return false;
    }
    const uint8_t safe_intensity = static_cast<uint8_t>(std::min<int>(intensity_percent, 100));
    const auto direction = direction_.load(std::memory_order_relaxed);
    bool applied = false;
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (!emergency_pending_.load() && pwm_ready_ &&
            active_until_us_.load(std::memory_order_relaxed) > esp_timer_get_time()) {
            switch (direction) {
                case Direction::kForward:
                    applied = SetMotor(left_in1_, left_in2_, false, safe_intensity) &&
                              SetMotor(right_in1_, right_in2_, false, safe_intensity);
                    break;
                case Direction::kBackward:
                    applied = SetMotor(left_in1_, left_in2_, true, safe_intensity) &&
                              SetMotor(right_in1_, right_in2_, true, safe_intensity);
                    break;
                case Direction::kLeft:
                    applied = SetMotor(left_in1_, left_in2_, true, safe_intensity) &&
                              SetMotor(right_in1_, right_in2_, false, safe_intensity);
                    break;
                case Direction::kRight:
                    applied = SetMotor(left_in1_, left_in2_, false, safe_intensity) &&
                              SetMotor(right_in1_, right_in2_, true, safe_intensity);
                    break;
            }
        }
    }
    if (applied) {
        active_intensity_percent_.store(safe_intensity, std::memory_order_relaxed);
    }
    return applied;
}

void MotorController::HandleTimerExpired(uint32_t generation) {
    if (generation != timer_generation_.load(std::memory_order_relaxed) || !available_ ||
        faulted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (phase_ == Phase::kDriving) {
        // The esp_timer callback has already removed power from both motors.
        phase_ = Phase::kIdle;
        moving_.store(false, std::memory_order_relaxed);
        active_until_us_.store(0, std::memory_order_relaxed);
        if (sequence_active_.load(std::memory_order_relaxed)) {
            sequence_completed_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!queued_commands_.empty()) {
            BeginDeadTime();
        } else {
            sequence_active_.store(false, std::memory_order_relaxed);
            PublishMotionActive(false);
        }
    } else if (phase_ == Phase::kDeadTime) {
        phase_ = Phase::kIdle;
        ApplyNextCommand();
    }
}

void MotorController::EnterFault(const char* reason) {
    timer_generation_.fetch_add(1, std::memory_order_relaxed);
    if (stop_timer_ != nullptr) {
        esp_timer_stop(stop_timer_);
    }
    AllOff();
    phase_ = Phase::kIdle;
    moving_.store(false, std::memory_order_relaxed);
    queued_commands_.clear();
    queued_runtime_ms_.store(0, std::memory_order_relaxed);
    queued_count_.store(0, std::memory_order_relaxed);
    sequence_active_.store(false, std::memory_order_relaxed);
    sequence_total_.store(0, std::memory_order_relaxed);
    sequence_completed_.store(0, std::memory_order_relaxed);
    active_until_us_.store(0, std::memory_order_relaxed);
    PublishMotionActive(false);
    faulted_.store(true, std::memory_order_relaxed);
    ESP_LOGE(TAG, "Motor control latched off: %s", reason);
}

void MotorController::Stop() {
    if (!available_) {
        return;
    }
    if (stop_timer_ != nullptr) {
        esp_timer_stop(stop_timer_);
    }
    timer_generation_.fetch_add(1, std::memory_order_relaxed);
    AllOff();
    phase_ = Phase::kIdle;
    moving_.store(false, std::memory_order_relaxed);
    queued_commands_.clear();
    queued_runtime_ms_.store(0, std::memory_order_relaxed);
    queued_count_.store(0, std::memory_order_relaxed);
    sequence_active_.store(false, std::memory_order_relaxed);
    sequence_total_.store(0, std::memory_order_relaxed);
    sequence_completed_.store(0, std::memory_order_relaxed);
    active_until_us_.store(0, std::memory_order_relaxed);
    PublishMotionActive(false);
    emergency_pending_.store(false);
}

void MotorController::EmergencyStop() {
    if (!available_) {
        return;
    }
    // Cut the H-bridge inputs from the caller's task immediately. Queue and
    // timer state are then normalized on the application task by Stop().
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        emergency_pending_.store(true);
        OutputsOffLocked();
        moving_.store(false, std::memory_order_relaxed);
        timer_generation_.fetch_add(1, std::memory_order_relaxed);
    }
    if (stop_timer_ != nullptr) {
        esp_timer_stop(stop_timer_);
    }
    Application::GetInstance().Schedule([this]() { Stop(); });
}

void MotorController::SetMotionGuard(std::function<bool(Direction)> guard) {
    motion_guard_ = std::move(guard);
}

void MotorController::SetMovementStateCallback(std::function<void(bool)> callback) {
    movement_state_callback_ = std::move(callback);
}

void MotorController::PublishMotionActive(bool active) {
    if (motion_active_.exchange(active, std::memory_order_relaxed) == active) {
        return;
    }
    if (movement_state_callback_) {
        movement_state_callback_(active);
    }
}

bool MotorController::IsMoving(Direction direction) const {
    return moving_.load(std::memory_order_relaxed) &&
           direction_.load(std::memory_order_relaxed) == direction;
}

std::string MotorController::StatusJson() const {
    static constexpr const char* kDirectionNames[] = {
        "forward",
        "backward",
        "left",
        "right",
    };
    const auto direction = direction_.load(std::memory_order_relaxed);
    const int64_t active_remaining_us =
        active_until_us_.load(std::memory_order_relaxed) - esp_timer_get_time();
    const uint32_t remaining_ms =
        queued_runtime_ms_.load(std::memory_order_relaxed) +
        (active_remaining_us > 0 ? static_cast<uint32_t>(active_remaining_us / 1000) : 0);
    char result[320];
    snprintf(
        result, sizeof(result),
        "{\"available\":%s,\"faulted\":%s,\"moving\":%s,\"direction\":\"%s\","
        "\"intensity_percent\":%u,\"queued\":%zu,\"remaining_ms\":%lu,\"sequence_active\":%s,"
        "\"sequence_total\":%zu,\"sequence_completed\":%zu}",
        available_ ? "true" : "false", faulted_.load(std::memory_order_relaxed) ? "true" : "false",
        moving_.load(std::memory_order_relaxed) ? "true" : "false",
        kDirectionNames[static_cast<int>(direction)],
        static_cast<unsigned>(active_intensity_percent_.load(std::memory_order_relaxed)),
        queued_count_.load(std::memory_order_relaxed), static_cast<unsigned long>(remaining_ms),
        sequence_active_.load(std::memory_order_relaxed) ? "true" : "false",
        sequence_total_.load(std::memory_order_relaxed),
        sequence_completed_.load(std::memory_order_relaxed));
    return result;
}

void MotorController::StopTimerCallback(void* arg) {
    auto* controller = static_cast<MotorController*>(arg);
    // Cut bridge drive immediately in the timer task. Scheduling only the
    // queue transition avoids leaving a motor powered if the main task stalls.
    controller->AllOff();
    const uint32_t generation = controller->timer_generation_.load(std::memory_order_relaxed);
    Application::GetInstance().Schedule(
        [controller, generation]() { controller->HandleTimerExpired(generation); });
}
