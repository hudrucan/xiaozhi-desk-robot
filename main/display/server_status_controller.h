#pragma once

#include "device_state.h"

#include <mutex>
#include <string>

struct cJSON;
class Display;

struct ServerStatusMessage {
    std::string state;
    std::string phase;
};

class ServerStatusController {
public:
    static bool Parse(const cJSON* root, ServerStatusMessage& message);

    void Handle(const ServerStatusMessage& message, Display* display, DeviceState device_state,
                bool asr_preparing);
    bool active() const;
    std::string active_phase() const;

private:
    static const char* BusyLabel(const std::string& phase);
    static const char* DeviceStateLabel(DeviceState state, bool asr_preparing);

    mutable std::mutex mutex_;
    std::string active_phase_;
};
