#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <atomic>
#include <memory>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    explicit WebsocketProtocol(bool persistent_candidate = false);
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    void EndConversation() override;
    void CloseTransport(bool reconnect = false) override;
    bool IsPersistentConnection() const override { return persistent_negotiated_.load(); }
    bool WantsPersistentConnection() const override { return persistent_candidate_.load(); }
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> persistent_candidate_{false};
    std::atomic<bool> persistent_negotiated_{false};
    std::atomic<bool> reconnect_allowed_{false};
    std::atomic<bool> reconnect_scheduled_{false};
    std::atomic<bool> connecting_{false};
    std::atomic<uint32_t> connection_generation_{0};
    std::atomic<ProtocolConnectionState> connection_state_{
        ProtocolConnectionState::kLegacy};
    std::atomic<int64_t> connection_started_us_{0};
    std::atomic<int64_t> last_server_rx_us_{0};
    std::atomic<uint32_t> reconnect_count_{0};
    std::atomic<uint32_t> reconnect_retry_delay_ms_{0};
    esp_timer_handle_t reconnect_timer_ = nullptr;
    esp_timer_handle_t heartbeat_timer_ = nullptr;
    uint32_t reconnect_delay_ms_ = 1000;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    void ScheduleReconnect();
    void HandleHeartbeat();
    void StopHeartbeat();
    void PublishDiagnostics() const;
};

#endif
