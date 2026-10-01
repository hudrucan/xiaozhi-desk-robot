#include "websocket_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <algorithm>
#include "assets/lang_config.h"

#define TAG "WS"

namespace {
constexpr uint64_t kHeartbeatIntervalUs = 30ULL * 1000 * 1000;
constexpr uint32_t kMaxReconnectDelayMs = 30000;
}

WebsocketProtocol::WebsocketProtocol(bool persistent_candidate) {
    persistent_candidate_.store(persistent_candidate);
    event_group_handle_ = xEventGroupCreate();

    esp_timer_create_args_t reconnect_args = {
        .callback = [](void* arg) {
            auto* protocol = static_cast<WebsocketProtocol*>(arg);
            protocol->reconnect_scheduled_.store(false);
            protocol->reconnect_retry_delay_ms_.store(0);
            auto alive = protocol->alive_;
            Application::GetInstance().Schedule([protocol, alive]() {
                if (*alive && protocol->reconnect_allowed_.load() &&
                    !protocol->IsAudioChannelOpened()) {
                    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        protocol->reconnect_count_.fetch_add(1);
                        protocol->PublishDiagnostics();
                        protocol->OpenAudioChannel();
                    } else {
                        protocol->ScheduleReconnect();
                    }
                }
            });
        },
        .arg = this,
        .name = "ws_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &reconnect_timer_));

    esp_timer_create_args_t heartbeat_args = {
        .callback = [](void* arg) {
            auto* protocol = static_cast<WebsocketProtocol*>(arg);
            auto alive = protocol->alive_;
            Application::GetInstance().Schedule([protocol, alive]() {
                if (*alive) {
                    protocol->HandleHeartbeat();
                }
            });
        },
        .arg = this,
        .name = "ws_heartbeat",
    };
    ESP_ERROR_CHECK(esp_timer_create(&heartbeat_args, &heartbeat_timer_));
}

WebsocketProtocol::~WebsocketProtocol() {
    *alive_ = false;
    reconnect_allowed_.store(false);
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }
    StopHeartbeat();
    if (heartbeat_timer_ != nullptr) {
        esp_timer_delete(heartbeat_timer_);
    }
    websocket_.reset();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    if (!persistent_candidate_.load()) {
        // Legacy WebSocket lifecycle remains turn-scoped.
        return true;
    }
    reconnect_allowed_.store(true);
    if (!OpenAudioChannel()) {
        ScheduleReconnect();
    }
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    if (persistent_negotiated_.load()) {
        if (send_goodbye) {
            EndConversation();
        }
        return;
    }
    CloseTransport(false);
}

void WebsocketProtocol::EndConversation() {
    if (!persistent_negotiated_.load()) {
        CloseTransport(false);
        return;
    }
    std::string message =
        "{\"session_id\":\"" + session_id_ + "\",\"type\":\"goodbye\"}";
    if (!SendText(message)) {
        CloseTransport(true);
    }
}

void WebsocketProtocol::CloseTransport(bool reconnect) {
    reconnect_allowed_.store(reconnect && persistent_candidate_.load());
    StopHeartbeat();
    connecting_.store(false);
    connection_started_us_.store(0);
    connection_state_.store(reconnect_allowed_.load()
                                ? ProtocolConnectionState::kReconnecting
                                : persistent_candidate_.load()
                                    ? ProtocolConnectionState::kDisconnected
                                    : ProtocolConnectionState::kLegacy);
    PublishDiagnostics();
    websocket_.reset();
    if (reconnect_allowed_.load()) {
        ScheduleReconnect();
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    if (IsAudioChannelOpened()) {
        return true;
    }
    bool expected = false;
    if (!connecting_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "Websocket connection attempt already in progress");
        return false;
    }
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    reconnect_scheduled_.store(false);
    reconnect_retry_delay_ms_.store(0);
    connection_state_.store(persistent_candidate_.load()
                                ? ProtocolConnectionState::kReconnecting
                                : ProtocolConnectionState::kLegacy);
    PublishDiagnostics();
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;
    last_incoming_time_ = std::chrono::steady_clock::now();

    auto network = Board::GetInstance().GetNetwork();
    const uint32_t generation = connection_generation_.fetch_add(1) + 1;
    websocket_.reset();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        connecting_.store(false);
        if (reconnect_allowed_.load()) {
            ScheduleReconnect();
        }
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this, generation](const char* data, size_t len, bool binary) {
        if (generation != connection_generation_.load()) {
            return;
        }
        if (!binary) {
            last_server_rx_us_.store(esp_timer_get_time());
            PublishDiagnostics();
        }
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)}));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)}));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)}));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGW(TAG, "Ignoring invalid JSON message");
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else if (strcmp(type->valuestring, "pong") == 0) {
                    ESP_LOGD(TAG, "Received websocket heartbeat pong");
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this, generation]() {
        if (!*alive_ || generation != connection_generation_.load()) {
            return;
        }
        ESP_LOGI(TAG, "Websocket disconnected");
        connecting_.store(false);
        connection_started_us_.store(0);
        connection_state_.store(reconnect_allowed_.load()
                                    ? ProtocolConnectionState::kReconnecting
                                    : persistent_candidate_.load()
                                        ? ProtocolConnectionState::kDisconnected
                                        : ProtocolConnectionState::kLegacy);
        PublishDiagnostics();
        StopHeartbeat();
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
        if (reconnect_allowed_.load()) {
            ScheduleReconnect();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (auto connected = websocket_->Connect(url.c_str()); !connected) {
        ESP_LOGE(TAG, "Failed to connect to websocket server: %s",
                 connected.error().ToString().c_str());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED, url);
        connecting_.store(false);
        if (reconnect_allowed_.load()) {
            ScheduleReconnect();
        }
        return false;
    }
    connection_started_us_.store(esp_timer_get_time());

    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        connecting_.store(false);
        CloseTransport(reconnect_allowed_.load());
        return false;
    }

    // Wait for server hello
    EventBits_t bits =
        xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE,
                            pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        connecting_.store(false);
        CloseTransport(reconnect_allowed_.load());
        return false;
    }

    connecting_.store(false);
    if (persistent_candidate_.load() && !persistent_negotiated_.load()) {
        ESP_LOGW(TAG, "Server did not negotiate persistent WebSocket; using legacy lifecycle");
        persistent_candidate_.store(false);
        reconnect_allowed_.store(false);
        connection_state_.store(ProtocolConnectionState::kLegacy);
        PublishDiagnostics();
        CloseTransport(false);
        return true;
    }

    if (persistent_negotiated_.load()) {
        reconnect_allowed_.store(true);
        reconnect_delay_ms_ = 1000;
        reconnect_retry_delay_ms_.store(0);
        connection_state_.store(ProtocolConnectionState::kConnected);
        PublishDiagnostics();
        esp_timer_stop(heartbeat_timer_);
        esp_timer_start_periodic(heartbeat_timer_, kHeartbeatIntervalUs);
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    if (on_connected_ != nullptr) {
        on_connected_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddBoolToObject(features, "status", true);
    cJSON_AddBoolToObject(features, "desk_robot_stt_partial_v1", true);
    cJSON_AddBoolToObject(features, "desk_robot_persistent_ws_v1",
                          persistent_candidate_.load());
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport)) {
        ESP_LOGE(TAG, "Missing or non-string transport in server hello");
        return;
    }
    if (strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    persistent_negotiated_.store(false);
    auto features = cJSON_GetObjectItem(root, "features");
    if (persistent_candidate_.load() && cJSON_IsObject(features)) {
        auto persistent = cJSON_GetObjectItem(features, "desk_robot_persistent_ws_v1");
        persistent_negotiated_.store(cJSON_IsTrue(persistent));
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

void WebsocketProtocol::ScheduleReconnect() {
    if (!reconnect_allowed_.load() || reconnect_timer_ == nullptr || !*alive_) {
        return;
    }
    bool expected = false;
    if (!reconnect_scheduled_.compare_exchange_strong(expected, true)) {
        return;
    }
    esp_timer_stop(reconnect_timer_);
    const uint32_t delay_ms = reconnect_delay_ms_;
    reconnect_retry_delay_ms_.store(delay_ms);
    reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2, kMaxReconnectDelayMs);
    connection_state_.store(ProtocolConnectionState::kReconnecting);
    PublishDiagnostics();
    ESP_LOGI(TAG, "Scheduling websocket reconnect in %u ms", delay_ms);
    esp_timer_start_once(reconnect_timer_, static_cast<uint64_t>(delay_ms) * 1000);
}

void WebsocketProtocol::HandleHeartbeat() {
    if (!persistent_negotiated_.load() || !IsAudioChannelOpened()) {
        if (persistent_negotiated_.load()) {
            ESP_LOGW(TAG, "Persistent websocket heartbeat timed out; reconnecting");
            CloseTransport(true);
        }
        return;
    }
    if (!SendText("{\"type\":\"ping\"}")) {
        CloseTransport(true);
    }
}

void WebsocketProtocol::StopHeartbeat() {
    if (heartbeat_timer_ != nullptr) {
        esp_timer_stop(heartbeat_timer_);
    }
}

void WebsocketProtocol::PublishDiagnostics() const {
    Application::GetInstance().UpdateServerTransportDiagnostics({
        .persistent = persistent_negotiated_.load(),
        .connection_state = connection_state_.load(),
        .connection_started_us = connection_started_us_.load(),
        .last_server_rx_us = last_server_rx_us_.load(),
        .reconnect_count = reconnect_count_.load(),
        .reconnect_delay_ms = reconnect_retry_delay_ms_.load(),
    });
}
