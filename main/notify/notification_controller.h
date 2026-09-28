#ifndef NOTIFICATION_CONTROLLER_H_
#define NOTIFICATION_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_timer.h>

#include "device_state.h"
#include "notify_player.h"

class Application;
class AudioService;

struct NotificationOptions {
    uint32_t display_hold_ms = 0;
    int presentation_duration_ms = 3500;
    bool chime = true;
    std::string reaction;
    std::string emotion;
    std::string oled_text;
};

class NotificationController {
public:
    NotificationController(Application& application, AudioService& audio_service);
    ~NotificationController();

    void Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
               NotificationOptions options = {});
    void Stop();
    void OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms);
    void OnPlaybackDrained();
    void OnDeviceStateChanged(DeviceState state);
    void InvalidateHeldSubtitleForDisplayOverride();
    bool HasHeldSubtitle() const;

private:
    Application& application_;
    AudioService& audio_service_;
    NotifyPlayer player_;
    uint32_t playback_id_ = 0;
    uint32_t display_hold_ms_ = 0;
    std::string latest_subtitle_;
    esp_timer_handle_t subtitle_hold_timer_ = nullptr;
    std::atomic_uint32_t subtitle_hold_generation_{0};
    std::atomic<int64_t> subtitle_hold_expires_at_us_{0};

    void HandleFinished(uint32_t playback_id, bool success);
    void ArmSubtitleHold(std::string subtitle, uint32_t hold_ms);
    void CancelSubtitleHold(bool clear_display);
    static void SubtitleHoldTimerCallback(void* arg);
    void HandleSubtitleHoldTimer();
};

#endif  // NOTIFICATION_CONTROLLER_H_
