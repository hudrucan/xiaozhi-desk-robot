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
    : application_(application), audio_service_(audio_service), player_(audio_service) {
    esp_timer_create_args_t timer_args = {
        .callback = SubtitleHoldTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notify_subtitle_hold",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &subtitle_hold_timer_) != ESP_OK) {
        subtitle_hold_timer_ = nullptr;
        ESP_LOGE(TAG, "Failed to create notification subtitle hold timer");
    }
}

NotificationController::~NotificationController() {
    if (subtitle_hold_timer_ != nullptr) {
        esp_timer_stop(subtitle_hold_timer_);
        esp_timer_delete(subtitle_hold_timer_);
    }
}

void NotificationController::Start(std::string audio_url,
                                   std::vector<NotifySubtitle> subtitles,
                                   NotificationOptions options) {
    if (application_.GetDeviceState() != kDeviceStateIdle || player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    CancelSubtitleHold(true);
    display_hold_ms_ = options.display_hold_ms;
    latest_subtitle_.clear();

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
    if (!options.reaction.empty() || !options.emotion.empty() || !options.oled_text.empty()) {
        if (!board.ApplyNotificationPresentation(options.reaction, options.emotion,
                                                 options.oled_text,
                                                 options.presentation_duration_ms)) {
            ESP_LOGW(TAG, "Notification presentation was ignored or rejected");
        }
    }
    if (options.chime) {
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }

    bool started = player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            application_.Schedule([this, id, text]() {
                if (application_.GetDeviceState() == kDeviceStateNotifying &&
                    playback_id_ == id) {
                    latest_subtitle_ = text;
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
    CancelSubtitleHold(true);
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

void NotificationController::OnDeviceStateChanged(DeviceState state) {
    if (state != kDeviceStateIdle && state != kDeviceStateNotifying) {
        CancelSubtitleHold(true);
    }
}

void NotificationController::InvalidateHeldSubtitleForDisplayOverride() {
    CancelSubtitleHold(true);
}

bool NotificationController::HasHeldSubtitle() const {
    return subtitle_hold_expires_at_us_.load() != 0;
}

void NotificationController::HandleFinished(uint32_t playback_id, bool success) {
    if (application_.GetDeviceState() != kDeviceStateNotifying ||
        playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    const bool should_hold = success && display_hold_ms_ > 0 && !latest_subtitle_.empty();
    const uint32_t hold_ms = display_hold_ms_;
    std::string held_subtitle = std::move(latest_subtitle_);
    display_hold_ms_ = 0;

    player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (!should_hold) {
        board.GetDisplay()->SetChatMessage("assistant", "");
    }
    if (should_hold) {
        ArmSubtitleHold(std::move(held_subtitle), hold_ms);
    }
    if (application_.GetDeviceState() == kDeviceStateNotifying) {
        application_.SetDeviceState(kDeviceStateIdle);
    }
}

void NotificationController::ArmSubtitleHold(std::string subtitle, uint32_t hold_ms) {
    if (subtitle.empty() || hold_ms == 0 || subtitle_hold_timer_ == nullptr) {
        return;
    }

    esp_timer_stop(subtitle_hold_timer_);
    subtitle_hold_generation_.fetch_add(1);
    const int64_t expires_at = esp_timer_get_time() + static_cast<int64_t>(hold_ms) * 1000;
    subtitle_hold_expires_at_us_.store(expires_at);
    if (esp_timer_start_once(subtitle_hold_timer_, static_cast<uint64_t>(hold_ms) * 1000) !=
        ESP_OK) {
        subtitle_hold_expires_at_us_.store(0);
        subtitle_hold_generation_.fetch_add(1);
        ESP_LOGE(TAG, "Failed to arm notification subtitle hold timer");
    }
}

void NotificationController::CancelSubtitleHold(bool clear_display) {
    const bool was_active = subtitle_hold_expires_at_us_.exchange(0) != 0;
    subtitle_hold_generation_.fetch_add(1);
    if (subtitle_hold_timer_ != nullptr) {
        esp_timer_stop(subtitle_hold_timer_);
    }
    if (was_active && clear_display) {
        Board::GetInstance().GetDisplay()->SetChatMessage("assistant", "");
    }
}

void NotificationController::SubtitleHoldTimerCallback(void* arg) {
    static_cast<NotificationController*>(arg)->HandleSubtitleHoldTimer();
}

void NotificationController::HandleSubtitleHoldTimer() {
    const uint32_t generation = subtitle_hold_generation_.load();
    const int64_t expires_at = subtitle_hold_expires_at_us_.load();
    if (expires_at == 0 || esp_timer_get_time() + 1000 < expires_at) {
        return;
    }
    application_.Schedule([this, generation, expires_at]() {
        if (subtitle_hold_generation_.load() != generation ||
            subtitle_hold_expires_at_us_.load() != expires_at) {
            return;
        }
        subtitle_hold_expires_at_us_.store(0);
        subtitle_hold_generation_.fetch_add(1);
        if (application_.GetDeviceState() == kDeviceStateIdle) {
            Board::GetInstance().GetDisplay()->SetChatMessage("assistant", "");
        }
    });
}
