#pragma once

#include <esp_http_server.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>

class RobotWebControlServer {
public:
    using ActionHandler =
        std::function<bool(const std::string&, int, const std::string&, std::string&)>;
    using StatusHandler = std::function<std::string()>;
    using SnapshotSender = std::function<bool(const uint8_t*, size_t)>;
    using SnapshotHandler = std::function<bool(const SnapshotSender&)>;
    using ChatProbeHandler = std::function<bool(const std::string&, std::string&)>;

    RobotWebControlServer(ActionHandler action_handler, StatusHandler status_handler,
                          SnapshotHandler snapshot_handler = {},
                          ChatProbeHandler chat_probe_handler = {});
    ~RobotWebControlServer();

    // Start buffering logs before Wi-Fi and the HTTP server are available.
    // Calling this more than once is safe.
    static void BeginLogCapture();

    void AppendConversationStatus(cJSON* root);
    void OnChatProbeEvent(const std::string& event, const std::string& text);

    bool Start(int port = 8080);
    void Stop();

private:
    static esp_err_t HandleRoot(httpd_req_t* request);
    static esp_err_t HandleStatus(httpd_req_t* request);
    static esp_err_t HandleLogs(httpd_req_t* request);
    static esp_err_t HandleAction(httpd_req_t* request);
    static esp_err_t HandleSnapshot(httpd_req_t* request);
    static esp_err_t HandleChatProbe(httpd_req_t* request);
    static esp_err_t HandleClearConversation(httpd_req_t* request);
    static esp_err_t SendJson(httpd_req_t* request, const char* status, const std::string& body);

    httpd_handle_t server_ = nullptr;
    ActionHandler action_handler_;
    StatusHandler status_handler_;
    SnapshotHandler snapshot_handler_;
    ChatProbeHandler chat_probe_handler_;

    struct ConversationMessage {
        uint32_t id;
        std::string role;
        std::string text;
    };
    std::mutex conversation_mutex_;
    std::vector<ConversationMessage> conversation_messages_;
    std::string conversation_state_{"Ready"};
    std::string conversation_error_;
    uint32_t next_conversation_id_ = 1;
    bool assistant_message_open_ = false;

    void TrimConversationLocked();
};
