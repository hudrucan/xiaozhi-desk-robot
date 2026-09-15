#include "robot_web_control_server.h"
#include "robot_web_control_page.h"

#include <esp_log.h>
#include <esp_log_write.h>
#include <cJSON.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#define TAG "RobotWebControl"

namespace {

constexpr size_t kLogBufferSize = 16 * 1024;
constexpr size_t kLogLineBufferSize = 768;
constexpr size_t kLogReadChunkSize = 4 * 1024;
constexpr size_t kChatProbeMaxCodepoints = 80;
constexpr size_t kConversationMaxBytes = 12 * 1024;
constexpr size_t kConversationMessageMaxBytes = 4 * 1024;

std::array<char, kLogBufferSize> log_buffer = {};
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
    log_stream_end += length;
    if (length >= kLogBufferSize) {
        std::memcpy(log_buffer.data(), data + length - kLogBufferSize, kLogBufferSize);
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
    std::memcpy(log_buffer.data() + write_at, data, first_length);
    if (first_length < length) {
        std::memcpy(log_buffer.data(), data + first_length, length - first_length);
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
    if (!log_capture_installed.exchange(true)) {
        previous_log_vprintf.store(esp_log_set_vprintf(CaptureLogVprintf));
    }
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
        error = "Text exceeds 80 UTF-8 codepoints";
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
    result.append(log_buffer.data() + read_at, first_length);
    if (first_length < length) {
        result.append(log_buffer.data(), length - first_length);
    }
    return result;
}

}  // namespace

RobotWebControlServer::RobotWebControlServer(ActionHandler action_handler,
                                             StatusHandler status_handler,
                                             SnapshotHandler snapshot_handler,
                                             ChatProbeHandler chat_probe_handler)
    : action_handler_(std::move(action_handler)),
      status_handler_(std::move(status_handler)),
      snapshot_handler_(std::move(snapshot_handler)),
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
    config.max_uri_handlers = 7;
    config.stack_size = 6144;

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
    const httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = HandleStatus,
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
    const httpd_uri_t chat_probe = {
        .uri = "/api/chat",
        .method = HTTP_POST,
        .handler = HandleChatProbe,
        .user_ctx = this,
    };
    const httpd_uri_t clear_conversation = {
        .uri = "/api/chat",
        .method = HTTP_DELETE,
        .handler = HandleClearConversation,
        .user_ctx = this,
    };
    if (httpd_register_uri_handler(server_, &root) != ESP_OK ||
        httpd_register_uri_handler(server_, &status) != ESP_OK ||
        httpd_register_uri_handler(server_, &action) != ESP_OK ||
        httpd_register_uri_handler(server_, &logs) != ESP_OK ||
        httpd_register_uri_handler(server_, &snapshot) != ESP_OK ||
        httpd_register_uri_handler(server_, &chat_probe) != ESP_OK ||
        httpd_register_uri_handler(server_, &clear_conversation) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register local control routes");
        Stop();
        return false;
    }
    ESP_LOGI(TAG, "Local control UI started on port %d", port);
    return true;
}

void RobotWebControlServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

void RobotWebControlServer::AppendConversationStatus(cJSON* root) {
    std::lock_guard<std::mutex> lock(conversation_mutex_);
    cJSON* conversation = cJSON_AddObjectToObject(root, "conversation");
    if (conversation == nullptr) {
        return;
    }
    cJSON_AddStringToObject(conversation, "state", conversation_state_.c_str());
    cJSON_AddStringToObject(conversation, "error", conversation_error_.c_str());
    cJSON* messages = cJSON_AddArrayToObject(conversation, "messages");
    if (messages == nullptr) {
        return;
    }
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

esp_err_t RobotWebControlServer::HandleStatus(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    return SendJson(request, "200 OK", self->status_handler_());
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
    const bool accepted =
        self->action_handler_(action->valuestring, control_value, control_text, message);
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
    if (request->content_len <= 0 || request->content_len > 512) {
        return SendJson(request, "400 Bad Request", R"({"ok":false,"message":"Invalid request"})");
    }

    std::array<char, 513> body = {};
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

esp_err_t RobotWebControlServer::HandleSnapshot(httpd_req_t* request) {
    auto* self = static_cast<RobotWebControlServer*>(request->user_ctx);
    if (!self->snapshot_handler_) {
        return SendJson(request, "503 Service Unavailable",
                        R"({"ok":false,"message":"Camera unavailable"})");
    }

    bool response_started = false;
    const bool captured = self->snapshot_handler_([&](const uint8_t* data, size_t length) {
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

esp_err_t RobotWebControlServer::SendJson(httpd_req_t* request, const char* status,
                                          const std::string& body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
}
