#include "robot_web_control_server.h"
#include "robot_web_control_page.h"
#include "asr_settings.h"
#include "control/robot_controller.h"
#include "web/robot_web_status_routes.h"

#include <esp_log.h>
#include <esp_log_write.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#define TAG "RobotWebControl"

namespace {

constexpr size_t kLogBufferSize = 16 * 1024;
constexpr size_t kLogLineBufferSize = 768;
constexpr size_t kLogReadChunkSize = 4 * 1024;
constexpr size_t kChatProbeMaxCodepoints = 512;
constexpr size_t kChatRequestMaxBytes = 4 * 1024;
constexpr size_t kAsrConfigRequestMaxBytes = 2 * 1024;
constexpr size_t kConversationMaxBytes = 12 * 1024;
constexpr size_t kConversationMessageMaxBytes = 4 * 1024;
constexpr uint32_t kCameraStreamTaskStackSize = 8192;
constexpr TickType_t kCameraStreamFrameDelay = pdMS_TO_TICKS(80);
constexpr char kCameraStreamContentType[] =
    "multipart/x-mixed-replace;boundary=xiaozhi-camera-frame";
constexpr char kCameraStreamBoundary[] = "--xiaozhi-camera-frame\r\n";

char* log_buffer = nullptr;
std::mutex log_mutex;
size_t log_buffer_start = 0;
size_t log_buffer_size = 0;
uint64_t log_stream_start = 0;
uint64_t log_stream_end = 0;
std::atomic<vprintf_like_t> previous_log_vprintf = nullptr;
std::atomic<bool> log_capture_installed = false;

void AppendLog(const char* data, size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_buffer == nullptr) {
        return;
    }
    log_stream_end += length;
    if (length >= kLogBufferSize) {
        std::memcpy(log_buffer, data + length - kLogBufferSize, kLogBufferSize);
        log_buffer_start = 0;
        log_buffer_size = kLogBufferSize;
        log_stream_start = log_stream_end - kLogBufferSize;
        return;
    }

    const size_t overflow =
        log_buffer_size + length > kLogBufferSize ? log_buffer_size + length - kLogBufferSize : 0;
    if (overflow > 0) {
        log_buffer_start = (log_buffer_start + overflow) % kLogBufferSize;
        log_buffer_size -= overflow;
        log_stream_start += overflow;
    }

    const size_t write_at = (log_buffer_start + log_buffer_size) % kLogBufferSize;
    const size_t first_length = std::min(length, kLogBufferSize - write_at);
    std::memcpy(log_buffer + write_at, data, first_length);
    if (first_length < length) {
        std::memcpy(log_buffer, data + first_length, length - first_length);
    }
    log_buffer_size += length;
}

int CaptureLogVprintf(const char* format, va_list args) {
    va_list output_args;
    va_copy(output_args, args);
    const auto output = previous_log_vprintf.load();
    const int result =
        output != nullptr ? output(format, output_args) : std::vprintf(format, output_args);
    va_end(output_args);

    std::array<char, kLogLineBufferSize> line = {};
    va_list capture_args;
    va_copy(capture_args, args);
    const int formatted = std::vsnprintf(line.data(), line.size(), format, capture_args);
    va_end(capture_args);
    if (formatted > 0) {
        const bool conversation_line =
            std::strstr(line.data(), "Application: << ") != nullptr ||
            std::strstr(line.data(), "Application: >> ") != nullptr ||
            std::strstr(line.data(), "TextChat tts/sentence_start text=") != nullptr ||
            std::strstr(line.data(), "TextChat incoming STT text=") != nullptr;
        if (!conversation_line) {
            AppendLog(line.data(), std::min(static_cast<size_t>(formatted), line.size() - 1));
        }
    }
    return result;
}

void InstallLogCapture() {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_capture_installed.load()) {
        return;
    }
    log_buffer = static_cast<char*>(
        heap_caps_calloc(kLogBufferSize, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (log_buffer == nullptr) {
        ESP_LOGE(TAG, "Web log capture disabled: PSRAM allocation failed");
        return;
    }
    previous_log_vprintf.store(esp_log_set_vprintf(CaptureLogVprintf));
    log_capture_installed.store(true);
}

bool DecodeUtf8Codepoint(const std::string& text, size_t& offset, uint32_t& codepoint) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    const uint8_t first = bytes[offset];
    size_t length = 0;
    if (first <= 0x7f) {
        codepoint = first;
        length = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
        codepoint = first & 0x1f;
        length = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
        codepoint = first & 0x0f;
        length = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
        codepoint = first & 0x07;
        length = 4;
    } else {
        return false;
    }
    if (offset + length > text.size()) {
        return false;
    }
    for (size_t index = 1; index < length; ++index) {
        if ((bytes[offset + index] & 0xc0) != 0x80) {
            return false;
        }
        codepoint = (codepoint << 6) | (bytes[offset + index] & 0x3f);
    }
    if ((length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
        return false;
    }
    offset += length;
    return true;
}

bool IsUnicodeWhitespace(uint32_t codepoint) {
    return codepoint == 0x20 || (codepoint >= 0x09 && codepoint <= 0x0d) || codepoint == 0x85 ||
           codepoint == 0xa0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
           codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f || codepoint == 0x3000;
}

bool NormalizeChatProbeText(const std::string& input, std::string& output, size_t& count,
                            std::string& error) {
    struct CodepointSpan {
        size_t begin;
        size_t end;
        bool whitespace;
    };
    std::vector<CodepointSpan> spans;
    size_t offset = 0;
    while (offset < input.size()) {
        const size_t begin = offset;
        uint32_t codepoint = 0;
        if (!DecodeUtf8Codepoint(input, offset, codepoint)) {
            error = "Text must be valid UTF-8";
            return false;
        }
        spans.push_back({begin, offset, IsUnicodeWhitespace(codepoint)});
    }
    size_t first = 0;
    while (first < spans.size() && spans[first].whitespace) {
        ++first;
    }
    size_t last = spans.size();
    while (last > first && spans[last - 1].whitespace) {
        --last;
    }
    if (first == last) {
        error = "Text is empty";
        return false;
    }
    count = last - first;
    if (count > kChatProbeMaxCodepoints) {
        error = "Text exceeds 512 UTF-8 codepoints";
        return false;
    }
    output.assign(input, spans[first].begin, spans[last - 1].end - spans[first].begin);
    return true;
}

void AppendUtf8Bounded(std::string& destination, const std::string& text, size_t maximum_size) {
    if (destination.size() >= maximum_size || text.empty()) {
        return;
    }
    size_t length = std::min(text.size(), maximum_size - destination.size());
    if (length < text.size()) {
        while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xc0) == 0x80) {
            --length;
        }
    }
    destination.append(text.data(), length);
}

std::string ReadLogs(uint64_t requested_cursor, uint64_t& next_cursor, bool& reset) {
    std::lock_guard<std::mutex> lock(log_mutex);
    if (log_buffer == nullptr) {
        next_cursor = 0;
        reset = false;
        return "[web log capture unavailable]\n";
    }
    reset = requested_cursor < log_stream_start || requested_cursor > log_stream_end;
    const uint64_t cursor = reset ? log_stream_start : requested_cursor;
    const size_t length = std::min(static_cast<size_t>(log_stream_end - cursor), kLogReadChunkSize);
    next_cursor = cursor + length;
    const size_t offset = static_cast<size_t>(cursor - log_stream_start);
    const size_t read_at = (log_buffer_start + offset) % kLogBufferSize;

    std::string result;
    result.reserve(length + (reset ? 24 : 0));
    if (reset) {
        result.append("[older logs dropped]\n");
    }
    const size_t first_length = std::min(length, kLogBufferSize - read_at);
    result.append(log_buffer + read_at, first_length);
    if (first_length < length) {
        result.append(log_buffer, length - first_length);
    }
    return result;
}

std::string EncodeAsrConfigResponse(bool ok, const char* message, const AsrConfig& config) {
    cJSON* response = cJSON_CreateObject();
    if (response == nullptr) {
        return R"({"ok":false,"message":"Out of memory"})";
    }
    cJSON_AddBoolToObject(response, "ok", ok);
    cJSON_AddStringToObject(response, "message", message);
    cJSON_AddStringToObject(response, "provider", AsrProviderName(config.provider));
    cJSON_AddBoolToObject(response, "gemini_configured", config.IsGeminiConfigured());
    char* encoded = cJSON_PrintUnformatted(response);
    const std::string result = encoded != nullptr ? encoded : R"({"ok":false})";
    cJSON_free(encoded);
    cJSON_Delete(response);
    return result;
}

std::string EncodeJson(cJSON* root, const char* fallback) {
    if (root == nullptr) {
        return fallback;
    }
    char* encoded = cJSON_PrintUnformatted(root);
    const std::string result = encoded != nullptr ? encoded : fallback;
    cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}

}  // namespace

struct RobotWebControlServer::CameraStreamContext {
    RobotWebControlServer* server;
    httpd_req_t* request;
};

RobotWebControlServer::RobotWebControlServer(RobotController& controller,
                                             ChatProbeHandler chat_probe_handler)
    : controller_(controller),
      robot_adapter_(controller),
      robot_status_(controller),
      chat_probe_handler_(std::move(chat_probe_handler)) {
    BeginLogCapture();
}

RobotWebControlServer::~RobotWebControlServer() { Stop(); }

void RobotWebControlServer::BeginLogCapture() { InstallLogCapture(); }

bool RobotWebControlServer::Start(int port) {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = 32770;
    // HTTPD_DEFAULT_CONFIG allows seven client sockets and reserves another
    // three for the server itself. With lwIP's default ten-socket pool that can
    // starve MQTT's UDP audio channel as soon as the browser reconnects. Four
    // clients are sufficient for status, logs, camera and one action request;
    // recycle the least-recently-used connection if another tab appears.
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.backlog_conn = 2;
    config.max_uri_handlers = 24;
    config.stack_size = 6144;
    config.send_wait_timeout = 2;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start local control server on port %d", port);
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleRoot,
        .user_ctx = this,
    };
    const httpd_uri_t action = {
        .uri = "/api/action",
        .method = HTTP_POST,
        .handler = HandleAction,
        .user_ctx = this,
    };
    const httpd_uri_t logs = {
        .uri = "/api/log",
        .method = HTTP_GET,
        .handler = HandleLogs,
        .user_ctx = this,
    };
    const httpd_uri_t snapshot = {
        .uri = "/api/camera/snapshot",
        .method = HTTP_GET,
        .handler = HandleSnapshot,
        .user_ctx = this,
    };
    const httpd_uri_t camera_stream = {
        .uri = "/api/camera/stream",
        .method = HTTP_GET,
        .handler = HandleCameraStream,
        .user_ctx = this,
    };
    const httpd_uri_t camera_mode = {
        .uri = "/api/camera/mode",
        .method = HTTP_POST,
        .handler = HandleCameraMode,
        .user_ctx = this,
    };
    const httpd_uri_t chat_probe = {
        .uri = "/api/chat",
        .method = HTTP_POST,
        .handler = HandleChatProbe,
        .user_ctx = this,
    };
    const httpd_uri_t get_conversation = {
        .uri = "/api/chat",
        .method = HTTP_GET,
        .handler = HandleGetConversation,
        .user_ctx = this,
    };
    const httpd_uri_t clear_conversation = {
        .uri = "/api/chat",
        .method = HTTP_DELETE,
        .handler = HandleClearConversation,
        .user_ctx = this,
    };
    const httpd_uri_t save_asr_config = {
        .uri = "/api/asr",
        .method = HTTP_POST,
        .handler = HandleSaveAsrConfig,
        .user_ctx = this,
    };
    const httpd_uri_t get_asr_config = {
        .uri = "/api/asr",
        .method = HTTP_GET,
        .handler = HandleGetAsrConfig,
        .user_ctx = this,
    };
    const httpd_uri_t clear_gemini_api_key = {
        .uri = "/api/asr",
        .method = HTTP_DELETE,
        .handler = HandleClearGeminiApiKey,
        .user_ctx = this,
    };
    const bool routes_ok = httpd_register_uri_handler(server_, &root) == ESP_OK &&
                           RegisterRobotWebStatusRoutes(server_, this, HandleDomainStatus);
    if (!routes_ok ||
        httpd_register_uri_handler(server_, &action) != ESP_OK ||
        httpd_register_uri_handler(server_, &logs) != ESP_OK ||
        httpd_register_uri_handler(server_, &snapshot) != ESP_OK ||
        httpd_register_uri_handler(server_, &camera_mode) != ESP_OK ||
        httpd_register_uri_handler(server_, &camera_stream) != ESP_OK ||
        httpd_register_uri_handler(server_, &get_conversation) != ESP_OK ||
        httpd_register_uri_handler(server_, &chat_probe) != ESP_OK ||
        httpd_register_uri_handler(server_, &clear_conversation) != ESP_OK ||
        httpd_register_uri_handler(server_, &get_asr_config) != ESP_OK ||
        httpd_register_uri_handler(server_, &save_asr_config) != ESP_OK ||
        httpd_register_uri_handler(server_, &clear_gemini_api_key) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register local control routes");
        Stop();
        return false;
    }
    ESP_LOGI(TAG, "Local control UI started on port %d", port);
    return true;
}

void RobotWebControlServer::Stop() {
    if (server_ != nullptr) {
        controller_.StopWebCameraStream();
        httpd_stop(server_);
        server_ = nullptr;
    }
}

std::string RobotWebControlServer::BuildDomainStatus(const char* uri) {
    return EncodeJson(CreateRobotWebDomainStatus(robot_status_, uri),
                      R"({"error":"out of memory"})");
}

std::string RobotWebControlServer::BuildConversationStatus() {
    std::lock_guard<std::mutex> lock(conversation_mutex_);
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return R"({"state":"Error","error":"out of memory","messages":[]})";
    }
    cJSON_AddStringToObject(root, "state", conversation_state_.c_str());
    cJSON_AddStringToObject(root, "error", conversation_error_.c_str());
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    if (messages != nullptr) {
        for (const auto& message : conversation_messages_) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                break;
            }
            cJSON_AddNumberToObject(item, "id", message.id);
            cJSON_AddStringToObject(item, "role", message.role.c_str());
            cJSON_AddStringToObject(item, "text", message.text.c_str());
            cJSON_AddItemToArray(messages, item);
        }
    }
    return EncodeJson(root, R"({"state":"Error","messages":[]})");
}

std::string RobotWebControlServer::BuildAsrStatus() {
    const AsrConfig config = AsrSettings::Load();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return R"({"provider":"xiaozhi","gemini_configured":false})";
    }
    cJSON_AddStringToObject(root, "provider", AsrProviderName(config.provider));
    cJSON_AddBoolToObject(root, "gemini_configured", config.IsGeminiConfigured());
    return EncodeJson(root, R"({"provider":"xiaozhi","gemini_configured":false})");
}

void RobotWebControlServer::TrimConversationLocked() {
    size_t total_bytes = 0;
    for (const auto& message : conversation_messages_) {
        total_bytes += message.text.size();
    }
    while (conversation_messages_.size() > 24 ||
           (total_bytes > kConversationMaxBytes && conversation_messages_.size() > 1)) {
        total_bytes -= conversation_messages_.front().text.size();
        conversation_messages_.erase(conversation_messages_.begin());
    }
}

void RobotWebControlServer::OnChatProbeEvent(const std::string& event, const std::string& text) {
    std::lock_guard<std::mutex> lock(conversation_mutex_);
    if (event == "sent") {
        conversation_state_ = "Waiting";
        conversation_error_.clear();
        assistant_message_open_ = false;
    } else if (event == "user") {
        conversation_messages_.push_back({next_conversation_id_++, "user", text});
        conversation_state_ = "Waiting";
        conversation_error_.clear();
        assistant_message_open_ = false;
    } else if (event == "speaking") {
        conversation_state_ = "Speaking";
    } else if (event == "assistant") {
        if (assistant_message_open_ && !conversation_messages_.empty() &&
            conversation_messages_.back().role == "assistant") {
            auto& current = conversation_messages_.back().text;
            if (!current.empty() && !text.empty() &&
                current.size() < kConversationMessageMaxBytes) {
                current.push_back(' ');
            }
            AppendUtf8Bounded(current, text, kConversationMessageMaxBytes);
        } else {
            std::string bounded_text;
            AppendUtf8Bounded(bounded_text, text, kConversationMessageMaxBytes);
            conversation_messages_.push_back(
                {next_conversation_id_++, "assistant", std::move(bounded_text)});
            assistant_message_open_ = true;
        }
    } else if (event == "completed") {
        conversation_state_ = "Ready";
        assistant_message_open_ = false;
    } else if (event == "error") {
        conversation_state_ = "Error";
        conversation_error_ = text;
        assistant_message_open_ = false;
    }
    TrimConversationLocked();
}

esp_err_t RobotWebControlServer::HandleRoot(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kRobotWebControlPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t RobotWebControlServer::HandleDomainStatus(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    return SendJson(request, "200 OK", self->BuildDomainStatus(request->uri));
}

esp_err_t RobotWebControlServer::HandleGetConversation(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    return SendJson(request, "200 OK", self->BuildConversationStatus());
}

esp_err_t RobotWebControlServer::HandleGetAsrConfig(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    return SendJson(request, "200 OK", self->BuildAsrStatus());
}

esp_err_t RobotWebControlServer::HandleLogs(httpd_req_t* request) {
    uint64_t cursor = 0;
    std::array<char, 64> query = {};
    std::array<char, 24> cursor_text = {};
    if (httpd_req_get_url_query_str(request, query.data(), query.size()) == ESP_OK &&
        httpd_query_key_value(query.data(), "since", cursor_text.data(), cursor_text.size()) ==
            ESP_OK) {
        char* end = nullptr;
        const auto parsed = std::strtoull(cursor_text.data(), &end, 10);
        if (end != cursor_text.data() && *end == '\0') {
            cursor = parsed;
        }
    }

    uint64_t next_cursor = 0;
    bool reset = false;
    const std::string logs = ReadLogs(cursor, next_cursor, reset);
    const std::string next_cursor_text = std::to_string(next_cursor);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Log-Cursor", next_cursor_text.c_str());
    httpd_resp_set_hdr(request, "X-Log-Reset", reset ? "1" : "0");
    return httpd_resp_send(request, logs.data(), logs.size());
}

esp_err_t RobotWebControlServer::HandleAction(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    if (request->content_len <= 0 || request->content_len > 256) {
        return SendJson(request, "400 Bad Request", R"({"ok":false,"message":"Invalid request"})");
    }

    std::array<char, 257> body = {};
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int result =
            httpd_req_recv(request, body.data() + received, request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), received);
    const cJSON* action =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "action") : nullptr;
    const cJSON* duration =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "duration_ms") : nullptr;
    const cJSON* value =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "value") : nullptr;
    const cJSON* text = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    if (!cJSON_IsString(action) || action->valuestring == nullptr) {
        cJSON_Delete(root);
        return SendJson(request, "400 Bad Request", R"({"ok":false,"message":"Missing action"})");
    }

    const int control_value = cJSON_IsNumber(value)
                                  ? value->valueint
                                  : (cJSON_IsNumber(duration) ? duration->valueint : 500);
    const std::string control_text =
        cJSON_IsString(text) && text->valuestring != nullptr ? text->valuestring : "";
    std::string message;
    const bool accepted = self->robot_adapter_.ExecuteAction(
        action->valuestring, control_value, control_text, message);
    cJSON_Delete(root);

    cJSON* response = cJSON_CreateObject();
    if (response == nullptr) {
        return SendJson(request, "500 Internal Server Error",
                        R"({"ok":false,"message":"Out of memory"})");
    }
    cJSON_AddBoolToObject(response, "ok", accepted);
    cJSON_AddStringToObject(response, "message", message.c_str());
    char* encoded = cJSON_PrintUnformatted(response);
    const std::string response_body = encoded != nullptr ? encoded : R"({"ok":false})";
    cJSON_free(encoded);
    cJSON_Delete(response);
    return SendJson(request, accepted ? "200 OK" : "400 Bad Request", response_body);
}

esp_err_t RobotWebControlServer::HandleChatProbe(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    if (!self->chat_probe_handler_) {
        return SendJson(request, "503 Service Unavailable",
                        R"({"ok":false,"message":"Text chat unavailable"})");
    }
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) > kChatRequestMaxBytes) {
        return SendJson(request, "400 Bad Request",
                        R"({"ok":false,"message":"Invalid request"})");
    }

    std::vector<char> body(static_cast<size_t>(request->content_len) + 1, '\0');
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int result =
            httpd_req_recv(request, body.data() + received, request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), received);
    const cJSON* text = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "text") : nullptr;
    if (!cJSON_IsString(text) || text->valuestring == nullptr) {
        cJSON_Delete(root);
        return SendJson(request, "400 Bad Request", R"({"ok":false,"message":"Missing text"})");
    }

    std::string normalized;
    std::string message;
    size_t codepoint_count = 0;
    const bool valid =
        NormalizeChatProbeText(text->valuestring, normalized, codepoint_count, message);
    cJSON_Delete(root);
    bool accepted = false;
    if (valid) {
        accepted = self->chat_probe_handler_(normalized, message);
    }
    if (accepted) {
        ESP_LOGI(TAG, "TextChat accepted chars=%u", static_cast<unsigned>(codepoint_count));
    } else {
        ESP_LOGW(TAG, "TextChat rejected reason=%s", message.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(self->conversation_mutex_);
        if (accepted) {
            self->conversation_messages_.push_back(
                {self->next_conversation_id_++, "user", normalized});
            self->TrimConversationLocked();
            self->conversation_state_ = "Sending";
            self->conversation_error_.clear();
            self->assistant_message_open_ = false;
        } else {
            self->conversation_state_ = "Error";
            self->conversation_error_ = message;
        }
    }

    cJSON* response = cJSON_CreateObject();
    if (response == nullptr) {
        return SendJson(request, "500 Internal Server Error",
                        R"({"ok":false,"message":"Out of memory"})");
    }
    cJSON_AddBoolToObject(response, "ok", accepted);
    cJSON_AddStringToObject(response, "message", message.c_str());
    if (valid) {
        cJSON_AddNumberToObject(response, "codepoints", codepoint_count);
    }
    char* encoded = cJSON_PrintUnformatted(response);
    const std::string response_body = encoded != nullptr ? encoded : R"({"ok":false})";
    cJSON_free(encoded);
    cJSON_Delete(response);
    return SendJson(request, accepted ? "202 Accepted" : "400 Bad Request", response_body);
}

esp_err_t RobotWebControlServer::HandleClearConversation(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    {
        std::lock_guard<std::mutex> lock(self->conversation_mutex_);
        self->conversation_messages_.clear();
        self->conversation_error_.clear();
        self->assistant_message_open_ = false;
        if (self->conversation_state_ == "Error") {
            self->conversation_state_ = "Ready";
        }
    }
    ESP_LOGI(TAG, "Conversation history cleared");
    return SendJson(request, "200 OK", R"({"ok":true,"message":"Conversation cleared"})");
}

esp_err_t RobotWebControlServer::HandleSaveAsrConfig(httpd_req_t* request) {
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) > kAsrConfigRequestMaxBytes) {
        return SendJson(request, "400 Bad Request",
                        R"({"ok":false,"message":"Invalid request"})");
    }

    std::vector<char> body(static_cast<size_t>(request->content_len) + 1, '\0');
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int result =
            httpd_req_recv(request, body.data() + received, request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), received);
    const cJSON* provider =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "provider") : nullptr;
    const cJSON* api_key =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "api_key") : nullptr;
    const bool has_provider = provider != nullptr;
    const bool has_api_key = api_key != nullptr;
    if (root == nullptr || (!has_provider && !has_api_key) ||
        (has_provider && (!cJSON_IsString(provider) || provider->valuestring == nullptr)) ||
        (has_api_key && (!cJSON_IsString(api_key) || api_key->valuestring == nullptr))) {
        cJSON_Delete(root);
        return SendJson(request, "400 Bad Request",
                        R"({"ok":false,"message":"Invalid ASR configuration"})");
    }

    AsrProvider selected_provider = AsrProvider::kXiaozhi;
    if (has_provider) {
        if (std::strcmp(provider->valuestring, "xiaozhi") == 0) {
            selected_provider = AsrProvider::kXiaozhi;
        } else if (std::strcmp(provider->valuestring, "gemini") == 0) {
            selected_provider = AsrProvider::kGemini;
        } else {
            cJSON_Delete(root);
            return SendJson(request, "400 Bad Request",
                            R"({"ok":false,"message":"Unknown ASR provider"})");
        }
    }

    // API-key updates are independent from provider selection. A blank/masked
    // value intentionally preserves the existing secret in AsrSettings.
    if (has_api_key) {
        AsrSettings::UpdateGeminiApiKey(api_key->valuestring);
    }
    cJSON_Delete(root);

    bool provider_saved = true;
    if (has_provider) {
        provider_saved = AsrSettings::SetProvider(selected_provider);
    }

    const AsrConfig config = AsrSettings::Load();
    const bool persisted = !has_provider || (provider_saved && config.provider == selected_provider);
    ESP_LOGI(TAG,
             "ASR config after update provider=%s configured=%d provider_requested=%d saved=%d",
             AsrProviderName(config.provider), config.IsGeminiConfigured() ? 1 : 0,
             has_provider ? 1 : 0, persisted ? 1 : 0);
    if (!persisted) {
        const char* message =
            selected_provider == AsrProvider::kGemini && !config.IsGeminiConfigured()
                ? "Gemini API key is not configured"
                : "ASR provider was not persisted";
        return SendJson(request, "400 Bad Request",
                        EncodeAsrConfigResponse(false, message, config));
    }

    const char* message = has_provider && has_api_key
                              ? "ASR settings saved"
                              : has_provider ? "ASR provider updated" : "Gemini API key saved";
    return SendJson(request, "200 OK", EncodeAsrConfigResponse(true, message, config));
}

esp_err_t RobotWebControlServer::HandleClearGeminiApiKey(httpd_req_t* request) {
    AsrSettings::ClearGeminiApiKey();
    const AsrConfig config = AsrSettings::Load();
    ESP_LOGI(TAG, "Gemini API key cleared; provider=%s", AsrProviderName(config.provider));

    return SendJson(request, "200 OK",
                    EncodeAsrConfigResponse(true, "Gemini API key cleared", config));
}

esp_err_t RobotWebControlServer::HandleSnapshot(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    bool response_started = false;
    const bool captured = self->controller_.SendSnapshot([&](const uint8_t* data, size_t length) {
        response_started = true;
        httpd_resp_set_type(request, "image/jpeg");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_send(request, reinterpret_cast<const char*>(data), length) == ESP_OK;
    });
    if (response_started) {
        return captured ? ESP_OK : ESP_FAIL;
    }
    return SendJson(request, "503 Service Unavailable",
                    R"({"ok":false,"message":"Camera capture failed or robot is busy"})");
}

esp_err_t RobotWebControlServer::HandleCameraMode(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    if (request->content_len <= 0 || request->content_len > 64) {
        return SendJson(request, "400 Bad Request",
                        R"({"ok":false,"message":"Invalid camera mode request"})");
    }

    std::array<char, 65> body = {};
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int result =
            httpd_req_recv(request, body.data() + received, request->content_len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(result);
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), received);
    const cJSON* mode =
        root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "mode") : nullptr;
    if (!cJSON_IsString(mode) || mode->valuestring == nullptr) {
        cJSON_Delete(root);
        return SendJson(request, "400 Bad Request",
                        R"({"ok":false,"message":"Missing camera mode"})");
    }

    const std::string requested_mode = mode->valuestring;
    cJSON_Delete(root);
    if (requested_mode == "web") {
        if (!self->controller_.StartWebCameraStream()) {
            return SendJson(request, "409 Conflict",
                            R"({"ok":false,"message":"Web Live requires Idle and an available camera"})");
        }
        return SendJson(request, "200 OK", R"({"ok":true,"mode":"web"})");
    }
    if (requested_mode == "off") {
        self->controller_.StopWebCameraStream();
        constexpr int kStopWaitSteps = 150;
        for (int step = 0;
             step < kStopWaitSteps && self->camera_stream_task_active_.load(); ++step) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (self->camera_stream_task_active_.load()) {
            return SendJson(request, "503 Service Unavailable",
                            R"({"ok":false,"message":"Camera stream is still stopping"})");
        }
        return SendJson(request, "200 OK", R"({"ok":true,"mode":"off"})");
    }
    return SendJson(request, "400 Bad Request",
                    R"({"ok":false,"message":"Mode must be web or off"})");
}

esp_err_t RobotWebControlServer::HandleCameraStream(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    if (!self->controller_.IsWebCameraStreamEnabled()) {
        return SendJson(request, "503 Service Unavailable",
                        R"({"ok":false,"message":"Web Live is off"})");
    }

    bool expected_inactive = false;
    if (!self->camera_stream_task_active_.compare_exchange_strong(expected_inactive, true)) {
        return SendJson(request, "409 Conflict",
                        R"({"ok":false,"message":"A camera stream client is already active"})");
    }

    auto context = std::unique_ptr<CameraStreamContext>(
        new (std::nothrow) CameraStreamContext{self, nullptr});
    if (context == nullptr) {
        self->camera_stream_task_active_.store(false);
        return SendJson(request, "503 Service Unavailable",
                        R"({"ok":false,"message":"Unable to allocate camera stream"})");
    }

    esp_err_t result = httpd_req_async_handler_begin(request, &context->request);
    if (result != ESP_OK) {
        self->camera_stream_task_active_.store(false);
        return SendJson(request, "503 Service Unavailable",
                        R"({"ok":false,"message":"Unable to start camera stream"})");
    }

    CameraStreamContext* task_context = context.release();
    const BaseType_t created =
        xTaskCreateWithCaps(CameraStreamTask, "camera_mjpeg", kCameraStreamTaskStackSize,
                            task_context, 1, nullptr, MALLOC_CAP_SPIRAM);
    if (created != pdPASS) {
        httpd_req_async_handler_complete(task_context->request);
        delete task_context;
        self->camera_stream_task_active_.store(false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void RobotWebControlServer::CameraStreamTask(void* context) {
    std::unique_ptr<CameraStreamContext> stream(
        static_cast<CameraStreamContext*>(context));
    ESP_LOGI(TAG, "Web MJPEG stream started");
    const esp_err_t result = stream->server->RunCameraStream(stream->request);
    stream->server->camera_stream_task_active_.store(false);
    const esp_err_t completed = httpd_req_async_handler_complete(stream->request);
    ESP_LOGI(TAG, "Web MJPEG stream stopped result=%s complete=%s",
             esp_err_to_name(result), esp_err_to_name(completed));
    stream.reset();
    vTaskDeleteWithCaps(nullptr);
}

esp_err_t RobotWebControlServer::RunCameraStream(httpd_req_t* request) {
    esp_err_t result = httpd_resp_set_type(request, kCameraStreamContentType);
    if (result != ESP_OK) {
        return result;
    }
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");

    size_t frame_count = 0;
    while (result == ESP_OK) {
        const bool sent = controller_.SendWebCameraFrame(
            [&](const uint8_t* data, size_t length) {
                char part_header[96];
                const int header_length = std::snprintf(
                    part_header, sizeof(part_header),
                    "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", length);
                if (header_length <= 0 ||
                    static_cast<size_t>(header_length) >= sizeof(part_header)) {
                    result = ESP_ERR_INVALID_SIZE;
                    return false;
                }
                result = httpd_resp_send_chunk(request, kCameraStreamBoundary,
                                               sizeof(kCameraStreamBoundary) - 1);
                if (result == ESP_OK) {
                    result = httpd_resp_send_chunk(request, part_header,
                                                   static_cast<size_t>(header_length));
                }
                if (result == ESP_OK) {
                    result = httpd_resp_send_chunk(
                        request, reinterpret_cast<const char*>(data), length);
                }
                if (result == ESP_OK) {
                    result = httpd_resp_send_chunk(request, "\r\n", 2);
                }
                return result == ESP_OK;
            });
        if (!sent) {
            break;
        }
        ++frame_count;
        vTaskDelay(kCameraStreamFrameDelay);
    }

    if (result == ESP_OK) {
        httpd_resp_send_chunk(request, nullptr, 0);
    }
    ESP_LOGI(TAG, "Web MJPEG frames=%zu", frame_count);
    return result;
}

esp_err_t RobotWebControlServer::SendJson(httpd_req_t* request, const char* status,
                                          const std::string& body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
}
