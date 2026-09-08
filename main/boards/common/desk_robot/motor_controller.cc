#include "motor_controller.h"

#include "application.h"

#include <driver/gpio.h>
#include <esp_log.h>

#include <cstdio>

#define TAG "MotorController"

MotorController::MotorController(gpio_num_t left_in1, gpio_num_t left_in2, gpio_num_t right_in1,
                                 gpio_num_t right_in2)
    : left_in1_(left_in1), left_in2_(left_in2), right_in1_(right_in1), right_in2_(right_in2) {
    const uint64_t pins =
        (1ULL << left_in1_) | (1ULL << left_in2_) | (1ULL << right_in1_) | (1ULL << right_in2_);
    gpio_config_t config = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    Stop();

    const esp_timer_create_args_t timer_args = {
        .callback = &MotorController::StopTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "motor_stop",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &stop_timer_));
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

void MotorController::Drive(Direction direction, uint32_t duration_ms) {
    if (queued_commands_.size() >= kMaxQueuedCommands) {
        ESP_LOGW(TAG, "Motor command queue is full; dropping command");
        return;
    }

    queued_commands_.push_back({direction, duration_ms});
    if (!moving_) {
        StartNextCommand();
    }
}

void MotorController::StartNextCommand() {
    if (queued_commands_.empty()) {
        gpio_set_level(left_in1_, 0);
        gpio_set_level(left_in2_, 0);
        gpio_set_level(right_in1_, 0);
        gpio_set_level(right_in2_, 0);
        moving_ = false;
        return;
    }

    const auto command = queued_commands_.front();
    queued_commands_.pop_front();
    direction_ = command.direction;
    moving_ = true;

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
            SetMotor(left_in1_, left_in2_, false);
            SetMotor(right_in1_, right_in2_, true);
            break;
        case Direction::kRight:
            SetMotor(left_in1_, left_in2_, true);
            SetMotor(right_in1_, right_in2_, false);
            break;
    }

    ESP_ERROR_CHECK(esp_timer_start_once(stop_timer_, command.duration_ms * 1000ULL));
}

void MotorController::Stop() {
    if (stop_timer_ != nullptr) {
        esp_timer_stop(stop_timer_);
    }
    gpio_set_level(left_in1_, 0);
    gpio_set_level(left_in2_, 0);
    gpio_set_level(right_in1_, 0);
    gpio_set_level(right_in2_, 0);
    moving_ = false;
    queued_commands_.clear();
}

std::string MotorController::StatusJson() const {
    static constexpr const char* kDirectionNames[] = {
        "forward",
        "backward",
        "left",
        "right",
    };
    char result[96];
    snprintf(result, sizeof(result), "{\"moving\":%s,\"direction\":\"%s\",\"queued\":%zu}",
             moving_ ? "true" : "false", kDirectionNames[static_cast<int>(direction_)],
             queued_commands_.size());
    return result;
}

void MotorController::StopTimerCallback(void* arg) {
    auto* controller = static_cast<MotorController*>(arg);
    Application::GetInstance().Schedule([controller]() { controller->StartNextCommand(); });
}
