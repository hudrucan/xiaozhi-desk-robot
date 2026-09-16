#include "notification_controller.h"

#include <utility>

#include <esp_log.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"

namespace {
const char* TAG = "NotificationController";
}  // namespace

NotificationController::NotificationController(Application& application,
                                               AudioService& audio_service)
    : application_(application), audio_service_(audio_service), player_(audio_service) {}

void NotificationController::Start(std::string audio_url,
                                   std::vector<NotifySubtitle> subtitles) {
    if (application_.GetDeviceState() != kDeviceStateIdle || player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!application_.SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++playback_id_;
    if (playback_id == 0) {
        playback_id = ++playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            application_.Schedule([this, id, text]() {
                if (application_.GetDeviceState() == kDeviceStateNotifying &&
                    playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            application_.Schedule([this, id, success]() { HandleFinished(id, success); });
        });

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        Stop();
    }
}

void NotificationController::Stop() {
    player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (application_.GetDeviceState() == kDeviceStateNotifying) {
        application_.SetDeviceState(kDeviceStateIdle);
    }
}

void NotificationController::OnPlaybackProgress(uint32_t playback_id,
                                                uint32_t media_position_ms) {
    player_.OnPlaybackProgress(playback_id, media_position_ms);
}

void NotificationController::OnPlaybackDrained() { player_.OnPlaybackDrained(); }

void NotificationController::HandleFinished(uint32_t playback_id, bool success) {
    if (application_.GetDeviceState() != kDeviceStateNotifying ||
        playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    Stop();
}
