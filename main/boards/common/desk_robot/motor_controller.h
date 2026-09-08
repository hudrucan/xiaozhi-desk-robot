#pragma once

#include <driver/gpio.h>
#include <esp_timer.h>

#include <deque>
#include <string>

class MotorController {
public:
    enum class Direction { kForward, kBackward, kLeft, kRight };

    MotorController(gpio_num_t left_in1, gpio_num_t left_in2, gpio_num_t right_in1,
                    gpio_num_t right_in2);
    ~MotorController();

    void Drive(Direction direction, uint32_t duration_ms);
    void Stop();
    std::string StatusJson() const;

private:
    struct Command {
        Direction direction;
        uint32_t duration_ms;
    };

    static constexpr size_t kMaxQueuedCommands = 16;

    static void StopTimerCallback(void* arg);
    void StartNextCommand();
    void SetMotor(gpio_num_t in1, gpio_num_t in2, bool forward);

    gpio_num_t left_in1_;
    gpio_num_t left_in2_;
    gpio_num_t right_in1_;
    gpio_num_t right_in2_;
    esp_timer_handle_t stop_timer_ = nullptr;
    Direction direction_ = Direction::kForward;
    bool moving_ = false;
    std::deque<Command> queued_commands_;
};
