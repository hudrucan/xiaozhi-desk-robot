#include "motor_controller.h"

#include "application.h"

#include <driver/gpio.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#define TAG "MotorController"

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

void MotorController::SetMotor(gpio_num_t in1, gpio_num_t in2, bool forward) {
    gpio_set_level(in1, forward ? 1 : 0);
    gpio_set_level(in2, forward ? 0 : 1);
}

void MotorController::AllOff() {
    if (!available_) {
        return;
    }
    gpio_set_level(left_in1_, 0);
    gpio_set_level(left_in2_, 0);
    gpio_set_level(right_in1_, 0);
    gpio_set_level(right_in2_, 0);
}

void MotorController::Drive(Direction direction, uint32_t duration_ms) {
    if (!available_ || faulted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (esp_timer_get_time() < arm_at_us_) {
        ESP_LOGW(TAG, "Ignoring motor command during startup arm delay");
        return;
    }
    if (motion_guard_ && !motion_guard_(direction)) {
        ESP_LOGW(TAG, "Motor command blocked by safety guard");
        return;
    }
    duration_ms = std::clamp(duration_ms, kMinDurationMs, kMaxDurationMs);
    if (queued_commands_.size() >= kMaxQueuedCommands) {
        ESP_LOGW(TAG, "Motor command queue is full; dropping command");
        return;
    }
    if (queued_runtime_ms_.load(std::memory_order_relaxed) + duration_ms > kMaxQueuedRuntimeMs) {
        ESP_LOGW(TAG, "Motor command queue runtime limit reached; dropping command");
        return;
    }

    queued_commands_.push_back({direction, duration_ms});
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
            {movement.direction, std::clamp(movement.duration_ms, kMinDurationMs, kMaxDurationMs)});
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
    phase_ = Phase::kDriving;
    moving_.store(true, std::memory_order_relaxed);
    active_until_us_.store(esp_timer_get_time() + static_cast<int64_t>(command.duration_ms) * 1000,
                           std::memory_order_relaxed);

    switch (command.direction) {
        case Direction::kForward:
            SetMotor(left_in1_, left_in2_, false);
            SetMotor(right_in1_, right_in2_, false);
            break;
        case Direction::kBackward:
            SetMotor(left_in1_, left_in2_, true);
            SetMotor(right_in1_, right_in2_, true);
            break;
        case Direction::kLeft:
            SetMotor(left_in1_, left_in2_, true);
            SetMotor(right_in1_, right_in2_, false);
            break;
        case Direction::kRight:
            SetMotor(left_in1_, left_in2_, false);
            SetMotor(right_in1_, right_in2_, true);
            break;
    }

    ArmTimer(command.duration_ms);
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
}

void MotorController::EmergencyStop() {
    if (!available_) {
        return;
    }
    // Cut the H-bridge inputs from the caller's task immediately. Queue and
    // timer state are then normalized on the application task by Stop().
    AllOff();
    moving_.store(false, std::memory_order_relaxed);
    timer_generation_.fetch_add(1, std::memory_order_relaxed);
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
    char result[280];
    snprintf(
        result, sizeof(result),
        "{\"available\":%s,\"faulted\":%s,\"moving\":%s,\"direction\":\"%s\","
        "\"queued\":%zu,\"remaining_ms\":%lu,\"sequence_active\":%s,"
        "\"sequence_total\":%zu,\"sequence_completed\":%zu}",
        available_ ? "true" : "false", faulted_.load(std::memory_order_relaxed) ? "true" : "false",
        moving_.load(std::memory_order_relaxed) ? "true" : "false",
        kDirectionNames[static_cast<int>(direction)], queued_count_.load(std::memory_order_relaxed),
        static_cast<unsigned long>(remaining_ms),
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
