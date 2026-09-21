#include "server_status_controller.h"

#include "assets/lang_config.h"
#include "display/display.h"

#include <cJSON.h>
#include <esp_log.h>

namespace {

constexpr char kTag[] = "ServerStatus";

}  // namespace

bool ServerStatusController::Parse(const cJSON* root, ServerStatusMessage& message) {
    const auto* state = cJSON_GetObjectItem(root, "state");
    const auto* phase = cJSON_GetObjectItem(root, "phase");
    if (!cJSON_IsString(state) || !cJSON_IsString(phase) || phase->valuestring[0] == '\0') {
        ESP_LOGW(kTag, "Status message requires state and phase");
        return false;
    }
    message.state = state->valuestring;
    message.phase = phase->valuestring;
    return true;
}

void ServerStatusController::Handle(const ServerStatusMessage& message, Display* display,
                                    DeviceState device_state, bool asr_preparing) {
    if (display == nullptr) {
        return;
    }
    if (message.state == "busy") {
        const char* label = BusyLabel(message.phase);
        if (label == nullptr) {
            ESP_LOGW(kTag, "Ignoring unsupported phase: %s", message.phase.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_phase_ = message.phase;
        }
        display->SetStatus(label);
        return;
    }
    if (message.state != "clear") {
        ESP_LOGW(kTag, "Ignoring unsupported state: %s", message.state.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_phase_ != message.phase) {
            return;
        }
        active_phase_.clear();
    }
    display->SetStatus(DeviceStateLabel(device_state, asr_preparing));
}

bool ServerStatusController::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_phase_.empty();
}

std::string ServerStatusController::active_phase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_phase_;
}

const char* ServerStatusController::BusyLabel(const std::string& phase) {
    if (phase == "initializing") {
        return Lang::Strings::INITIALIZING;
    }
    if (phase == "thinking") {
        return Lang::Strings::PROCESSING;
    }
    return nullptr;
}

const char* ServerStatusController::DeviceStateLabel(DeviceState state, bool asr_preparing) {
    switch (state) {
        case kDeviceStateStarting:
            return Lang::Strings::INITIALIZING;
        case kDeviceStateConnecting:
            return Lang::Strings::CONNECTING;
        case kDeviceStateListening:
            return asr_preparing ? Lang::Strings::PREPARING_ASR : Lang::Strings::LISTENING;
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            return Lang::Strings::SPEAKING;
        case kDeviceStateActivating:
            return Lang::Strings::ACTIVATION;
        case kDeviceStateUpgrading:
            return Lang::Strings::UPGRADING;
        case kDeviceStateWifiConfiguring:
        case kDeviceStateAudioTesting:
            return Lang::Strings::WIFI_CONFIG_MODE;
        case kDeviceStateFatalError:
            return Lang::Strings::ERROR;
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
        default:
            return Lang::Strings::STANDBY;
    }
}
