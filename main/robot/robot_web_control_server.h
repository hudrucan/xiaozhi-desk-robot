#pragma once

#include "web/robot_web_adapter.h"
#include "web/robot_web_status.h"

#include <esp_http_server.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>

class RobotController;

class RobotWebControlServer {
public:
    using ChatProbeHandler = std::function<bool(const std::string&, std::string&)>;

    RobotWebControlServer(RobotController& controller, ChatProbeHandler chat_probe_handler = {});
    ~RobotWebControlServer();

    // Start buffering logs before Wi-Fi and the HTTP server are available.
    // Calling this more than once is safe.
    static void BeginLogCapture();

    void OnChatProbeEvent(const std::string& event, const std::string& text);

    bool Start(int port = 8080);
    void Stop();

private:
    static esp_err_t HandleRoot(httpd_req_t* request);
    static esp_err_t HandleStatus(httpd_req_t* request);
    static esp_err_t HandleDomainStatus(httpd_req_t* request);
    static esp_err_t HandleGetConversation(httpd_req_t* request);
    static esp_err_t HandleGetAsrConfig(httpd_req_t* request);
    static esp_err_t HandleLogs(httpd_req_t* request);
    static esp_err_t HandleAction(httpd_req_t* request);
    static esp_err_t HandleSnapshot(httpd_req_t* request);
    static esp_err_t HandleChatProbe(httpd_req_t* request);
    static esp_err_t HandleClearConversation(httpd_req_t* request);
    static esp_err_t HandleSaveAsrConfig(httpd_req_t* request);
    static esp_err_t HandleClearGeminiApiKey(httpd_req_t* request);
    static esp_err_t SendJson(httpd_req_t* request, const char* status, const std::string& body);
    std::string BuildStatus();
    std::string BuildDomainStatus(const char* uri);
    std::string BuildConversationStatus();
    std::string BuildAsrStatus();
    void AppendConversationStatus(cJSON* root);
    void AppendAsrStatus(cJSON* root);

    httpd_handle_t server_ = nullptr;
    RobotController& controller_;
    RobotWebAdapter robot_adapter_;
    RobotWebStatus robot_status_;
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
