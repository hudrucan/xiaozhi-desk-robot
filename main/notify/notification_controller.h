#ifndef NOTIFICATION_CONTROLLER_H_
#define NOTIFICATION_CONTROLLER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "notify_player.h"

class Application;
class AudioService;

class NotificationController {
public:
    NotificationController(Application& application, AudioService& audio_service);

    void Start(std::string audio_url, std::vector<NotifySubtitle> subtitles);
    void Stop();
    void OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms);
    void OnPlaybackDrained();

private:
    Application& application_;
    AudioService& audio_service_;
    NotifyPlayer player_;
    uint32_t playback_id_ = 0;

    void HandleFinished(uint32_t playback_id, bool success);
};

#endif  // NOTIFICATION_CONTROLLER_H_
