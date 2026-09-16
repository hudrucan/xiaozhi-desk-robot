#include "gemini_transcribe_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <web_socket.h>

#include "board.h"

#define TAG "GeminiASR"

namespace {

constexpr char kModel[] = "models/gemini-3.5-transcribe-live";
constexpr char kWebSocketEndpoint[] =
    "wss://generativelanguage.googleapis.com/ws/"
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
constexpr char kAudioMimeType[] = "audio/pcm;rate=16000";
constexpr size_t kMaxVocabularyTerms = 1000;

#if defined(CONFIG_LOG_DYNAMIC_LEVEL_CONTROL) && CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
std::mutex sensitive_websocket_connect_mutex;

class ScopedWebSocketLogSilencer {
public:
    ScopedWebSocketLogSilencer() : previous_level_(esp_log_level_get("WebSocket")) {
        esp_log_level_set("WebSocket", ESP_LOG_NONE);
    }

    ~ScopedWebSocketLogSilencer() { esp_log_level_set("WebSocket", previous_level_); }

private:
    esp_log_level_t previous_level_;
};
#endif

void SecureClear(std::string& value) {
    std::fill(value.begin(), value.end(), '\0');
    value.clear();
}

std::string RedactSecret(std::string value, std::string_view secret) {
    if (secret.empty()) {
        return value;
    }
    size_t offset = 0;
    while ((offset = value.find(secret, offset)) != std::string::npos) {
        value.replace(offset, secret.size(), "[redacted]");
        offset += std::strlen("[redacted]");
    }
    return value;
}

std::string_view TrimWhitespace(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool ParseVocabulary(const std::string& compact, std::vector<std::string>& terms,
                     std::string& error) {
    size_t begin = 0;
    while (begin <= compact.size()) {
        size_t end = compact.find_first_of(",\n", begin);
        if (end == std::string::npos) {
            end = compact.size();
        }
        const auto term = TrimWhitespace(std::string_view(compact).substr(begin, end - begin));
        if (!term.empty()) {
            if (terms.size() >= kMaxVocabularyTerms) {
                error = "Gemini vocabulary exceeds 1000 terms";
                return false;
            }
            terms.emplace_back(term);
        }
        if (end == compact.size()) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool Base64Encode(const int16_t* samples, size_t sample_count, std::string& encoded) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(samples);
    const size_t byte_count = sample_count * sizeof(int16_t);
    const size_t encoded_capacity = 4 * ((byte_count + 2) / 3);
    // mbedtls_base64_encode() also writes a trailing NUL byte.
    encoded.resize(encoded_capacity + 1);
    size_t encoded_size = 0;
    const int result = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &encoded_size, bytes,
        byte_count);
    if (result != 0) {
        encoded.clear();
        return false;
    }
    encoded.resize(encoded_size);
    return true;
}

std::string TrimTranscript(const char* text) {
    if (text == nullptr) {
        return {};
    }
    const std::string_view trimmed = TrimWhitespace(text);
    return std::string(trimmed);
}

}  // namespace

GeminiTranscribeClient::GeminiTranscribeClient() {
    worker_events_ = xEventGroupCreate();
}

GeminiTranscribeClient::~GeminiTranscribeClient() {
    Cancel();
    if (worker_events_ != nullptr && worker_created_.load() &&
        (xEventGroupGetBits(worker_events_) & kWorkerStopped) == 0) {
        xEventGroupWaitBits(worker_events_, kWorkerStopped, pdFALSE, pdTRUE, portMAX_DELAY);
    }
    if (worker_events_ != nullptr) {
        vEventGroupDelete(worker_events_);
    }
}

bool GeminiTranscribeClient::Start(const AsrConfig& config, Callbacks callbacks) {
#if !defined(CONFIG_LOG_DYNAMIC_LEVEL_CONTROL) || !CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
    (void)config;
    (void)callbacks;
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = "Gemini ASR is unavailable because dynamic log control is disabled";
    state_.store(State::kError);
    return false;
#else
    if (!config.IsGeminiConfigured()) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = "Gemini API key is not configured";
        state_.store(State::kError);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_events_ == nullptr) {
        last_error_ = "Could not create Gemini worker event group";
        state_.store(State::kError);
        return false;
    }
    if (worker_running_.load() ||
        (worker_created_.load() &&
         (xEventGroupGetBits(worker_events_) & kWorkerStopped) == 0)) {
        last_error_ = "Gemini ASR session is already running";
        return false;
    }

    xEventGroupClearBits(worker_events_, kWorkerStopped);
    pcm_queue_.clear();
    message_queue_.clear();
    config_ = config;
    callbacks_ = std::move(callbacks);
    last_error_.clear();
    network_error_.clear();
    cancel_requested_ = false;
    audio_stream_end_requested_ = false;
    remote_disconnected_ = false;
    setup_complete_ = false;
    final_callback_sent_ = false;
    pcm_drop_count_.store(0);
    connect_latency_ms_.store(0);
    state_.store(State::kIdle);
    worker_running_.store(true);
    worker_created_.store(true);

    const BaseType_t created =
        xTaskCreate(WorkerTask, "gemini_asr", 8192, this, 3, nullptr);
    if (created != pdPASS) {
        worker_running_.store(false);
        worker_created_.store(false);
        SecureClear(config_.gemini_api_key);
        last_error_ = "Could not create Gemini ASR worker";
        state_.store(State::kError);
        return false;
    }
    return true;
#endif
}

bool GeminiTranscribeClient::PushPcm(std::vector<int16_t>&& pcm) {
    if (pcm.empty() || pcm.size() > kMaxPcmFrameSamples) {
        return false;
    }
    const State current_state = state_.load();
    if (current_state != State::kReady && current_state != State::kStreaming) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_requested_ || audio_stream_end_requested_) {
        return false;
    }
    if (pcm_queue_.full()) {
        pcm_queue_.pop_front();
        pcm_drop_count_.fetch_add(1);
    }
    pcm_queue_.push_back(std::move(pcm));
    cv_.notify_one();
    return true;
}

bool GeminiTranscribeClient::SendAudioStreamEnd() {
    const State current_state = state_.load();
    if (current_state != State::kReady && current_state != State::kStreaming) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_requested_ || audio_stream_end_requested_) {
        return false;
    }
    audio_stream_end_requested_ = true;
    cv_.notify_one();
    return true;
}

void GeminiTranscribeClient::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_running_.load()) {
        return;
    }
    cancel_requested_ = true;
    cv_.notify_one();
}

std::string GeminiTranscribeClient::GetLastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

const char* GeminiTranscribeClient::StateName(State state) {
    switch (state) {
        case State::kIdle:
            return "idle";
        case State::kConnecting:
            return "connecting";
        case State::kConnected:
            return "connected";
        case State::kAwaitingSetup:
            return "awaiting_setup";
        case State::kReady:
            return "ready";
        case State::kStreaming:
            return "streaming";
        case State::kEnding:
            return "ending";
        case State::kFinalized:
            return "finalized";
        case State::kClosed:
            return "closed";
        case State::kError:
            return "error";
    }
    return "unknown";
}

void GeminiTranscribeClient::WorkerTask(void* argument) {
    auto* client = static_cast<GeminiTranscribeClient*>(argument);
    client->WorkerLoop();
    client->worker_running_.store(false);
    xEventGroupSetBits(client->worker_events_, kWorkerStopped);
    vTaskDelete(nullptr);
}

void GeminiTranscribeClient::WorkerLoop() {
    bool failed = false;
    bool final_received = false;
    std::vector<int16_t> pending_pcm;
    pending_pcm.reserve(kSendChunkSamples + kMaxPcmFrameSamples);

    SetState(State::kConnecting);
    ESP_LOGI(TAG, "Gemini ASR connecting");

    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        Fail("Network interface is unavailable");
        failed = true;
    } else {
        websocket_ = network->CreateWebSocket(2);
        if (websocket_ == nullptr) {
            Fail("Could not create Gemini WebSocket");
            failed = true;
        }
    }

    if (!failed) {
        // The current WebSocket implementation reassembles fragmented frames and
        // invokes OnData only for a complete message.
        websocket_->OnData([this](const char* data, size_t length, bool binary) {
            QueueServerMessage(data, length, binary);
        });
        websocket_->OnError([this](const NetworkError& error) {
            QueueNetworkError(error.ToString());
        });
        websocket_->OnDisconnected([this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            remote_disconnected_ = true;
            cv_.notify_one();
        });

        std::string url = kWebSocketEndpoint;
        url += config_.gemini_api_key;
#if defined(CONFIG_LOG_DYNAMIC_LEVEL_CONTROL) && CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
        const int64_t connect_start_us = esp_timer_get_time();
        NetworkResult<> connected;
        {
            std::lock_guard<std::mutex> connect_lock(sensitive_websocket_connect_mutex);
            ScopedWebSocketLogSilencer silence_sensitive_url;
            connected = websocket_->Connect(url.c_str());
        }
        connect_latency_ms_.store(
            static_cast<uint32_t>((esp_timer_get_time() - connect_start_us) / 1000));
        SecureClear(url);

        if (IsCancelRequested()) {
            // Cancellation during the blocking TLS/WebSocket handshake is handled
            // immediately after Connect() returns; socket teardown remains worker-owned.
        } else if (!connected) {
            Fail(std::string("Gemini WebSocket connection failed: ") +
                 connected.error().ToString());
            failed = true;
        } else {
            SetState(State::kConnected);
            ESP_LOGI(TAG, "Gemini ASR connected");
        }
#else
        SecureClear(url);
        Fail("Gemini ASR is unavailable because dynamic log control is disabled");
        failed = true;
#endif
    }

    int64_t setup_deadline_us = 0;
    if (!failed && !IsCancelRequested()) {
        std::string setup_error;
        const std::string setup_message = BuildSetupMessage(setup_error);
        if (setup_message.empty()) {
            Fail(setup_error.empty() ? "Could not build Gemini setup" : setup_error);
            failed = true;
        } else if (!websocket_->Send(setup_message)) {
            Fail("Could not send Gemini setup");
            failed = true;
        } else {
            setup_deadline_us = esp_timer_get_time() + kSetupTimeoutUs;
            SetState(State::kAwaitingSetup);
        }
    }

    while (!failed && !final_received && !IsCancelRequested()) {
        std::string server_message;
        std::vector<int16_t> pcm;
        std::string network_error;
        bool disconnected = false;
        bool send_stream_end = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return cancel_requested_ || !message_queue_.empty() || !network_error_.empty() ||
                       remote_disconnected_ || (setup_complete_ && !pcm_queue_.empty()) ||
                       (setup_complete_ && audio_stream_end_requested_);
            });

            if (cancel_requested_) {
                break;
            }
            if (!network_error_.empty()) {
                network_error = std::move(network_error_);
                network_error_.clear();
            } else if (!message_queue_.empty()) {
                server_message = std::move(message_queue_.front());
                message_queue_.pop_front();
            } else if (remote_disconnected_) {
                disconnected = true;
            } else if (setup_complete_ && !pcm_queue_.empty()) {
                pcm = std::move(pcm_queue_.front());
                pcm_queue_.pop_front();
            } else if (setup_complete_ && audio_stream_end_requested_) {
                send_stream_end = true;
            }
        }

        if (!server_message.empty()) {
            const auto result = HandleServerMessage(server_message);
            if (result == ServerMessageResult::kFinalReceived) {
                final_received = true;
            } else if (result == ServerMessageResult::kError) {
                failed = true;
            }
            continue;
        }
        if (!network_error.empty()) {
            Fail(std::string("Gemini WebSocket error: ") + network_error);
            failed = true;
            continue;
        }
        if (disconnected) {
            Fail("Gemini WebSocket disconnected");
            failed = true;
            continue;
        }
        if (!setup_complete_ && setup_deadline_us > 0 &&
            esp_timer_get_time() >= setup_deadline_us) {
            Fail("Gemini setup timed out");
            failed = true;
            continue;
        }
        if (!pcm.empty()) {
            pending_pcm.insert(pending_pcm.end(), pcm.begin(), pcm.end());
            while (pending_pcm.size() >= kSendChunkSamples) {
                if (!SendPcmChunk(pending_pcm.data(), kSendChunkSamples)) {
                    Fail("Could not send Gemini PCM audio");
                    failed = true;
                    break;
                }
                pending_pcm.erase(pending_pcm.begin(),
                                  pending_pcm.begin() + kSendChunkSamples);
            }
            continue;
        }
        if (send_stream_end) {
            if (!pending_pcm.empty()) {
                if (!SendPcmChunk(pending_pcm.data(), pending_pcm.size())) {
                    Fail("Could not flush Gemini PCM audio");
                    failed = true;
                    continue;
                }
                pending_pcm.clear();
            }
            if (!websocket_->Send(R"({"realtimeInput":{"audioStreamEnd":true}})")) {
                Fail("Could not send Gemini audioStreamEnd");
                failed = true;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_stream_end_requested_ = false;
            }
            SetState(State::kEnding);
            ESP_LOGI(TAG, "Gemini ASR audioStreamEnd sent");
        }
    }

    CloseSocket();
    SecureClear(config_.gemini_api_key);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm_queue_.clear();
        message_queue_.clear();
        network_error_.clear();
        audio_stream_end_requested_ = false;
        setup_complete_ = false;
    }

    if (!failed) {
        SetState(State::kClosed);
    }
    ESP_LOGI(TAG, "Gemini ASR session closed");
}

void GeminiTranscribeClient::QueueServerMessage(const char* data, size_t length, bool binary) {
    if (binary) {
        QueueNetworkError("Unexpected binary response");
        return;
    }
    if (data == nullptr || length == 0 || length > kMaxServerMessageBytes) {
        QueueNetworkError("Invalid Gemini response size");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (message_queue_.full()) {
        if (network_error_.empty()) {
            network_error_ = "Gemini response queue overflow";
        }
        cv_.notify_one();
        return;
    }
    message_queue_.push_back(std::string(data, length));
    cv_.notify_one();
}

void GeminiTranscribeClient::QueueNetworkError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (network_error_.empty()) {
        network_error_ = error;
    }
    cv_.notify_one();
}

void GeminiTranscribeClient::SetState(State state) {
    state_.store(state);
    if (callbacks_.on_state) {
        callbacks_.on_state(state);
    }
}

void GeminiTranscribeClient::Fail(const std::string& error) {
    const std::string safe_error = RedactSecret(error, config_.gemini_api_key);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = safe_error;
    }
    ESP_LOGE(TAG, "%s", safe_error.c_str());
    SetState(State::kError);
    if (callbacks_.on_error) {
        callbacks_.on_error(safe_error);
    }
}

bool GeminiTranscribeClient::IsCancelRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancel_requested_;
}

bool GeminiTranscribeClient::SendPcmChunk(const int16_t* samples, size_t sample_count) {
    std::string encoded;
    if (!Base64Encode(samples, sample_count, encoded)) {
        return false;
    }

    std::string message;
    message.reserve(encoded.size() + 96);
    message.append(R"({"realtimeInput":{"audio":{"data":")");
    message.append(encoded);
    message.append(R"(","mimeType":")");
    message.append(kAudioMimeType);
    message.append(R"("}}})");

    if (!websocket_->Send(message)) {
        return false;
    }
    if (state_.load() == State::kReady) {
        SetState(State::kStreaming);
    }
    return true;
}

GeminiTranscribeClient::ServerMessageResult GeminiTranscribeClient::HandleServerMessage(
    const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.data(), message.size());
    if (root == nullptr) {
        Fail("Gemini returned invalid JSON");
        return ServerMessageResult::kError;
    }

    const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        const cJSON* detail = cJSON_GetObjectItemCaseSensitive(error, "message");
        const std::string safe_error =
            cJSON_IsString(detail) && detail->valuestring != nullptr
                ? std::string("Gemini rejected the session: ") + detail->valuestring
                : "Gemini rejected the session";
        cJSON_Delete(root);
        Fail(safe_error);
        return ServerMessageResult::kError;
    }

    const cJSON* setup_complete = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (cJSON_IsObject(setup_complete) && !setup_complete_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            setup_complete_ = true;
        }
        SetState(State::kReady);
        ESP_LOGI(TAG, "Gemini ASR setup complete");
    }

    const cJSON* server_content = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (cJSON_IsObject(server_content)) {
        const cJSON* interim =
            cJSON_GetObjectItemCaseSensitive(server_content, "interimInputTranscription");
        if (cJSON_IsObject(interim)) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(interim, "text");
            if (cJSON_IsString(text) && text->valuestring != nullptr &&
                callbacks_.on_interim_transcript) {
                callbacks_.on_interim_transcript(text->valuestring);
            }
        }

        const cJSON* final =
            cJSON_GetObjectItemCaseSensitive(server_content, "inputTranscription");
        if (cJSON_IsObject(final) && !final_callback_sent_) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(final, "text");
            const std::string transcript =
                cJSON_IsString(text) ? TrimTranscript(text->valuestring) : std::string();
            if (!transcript.empty()) {
                final_callback_sent_ = true;
                SetState(State::kFinalized);
                ESP_LOGI(TAG, "Gemini ASR final transcript received");
                if (callbacks_.on_final_transcript) {
                    callbacks_.on_final_transcript(transcript);
                }
                cJSON_Delete(root);
                return ServerMessageResult::kFinalReceived;
            }
        }
    }

    cJSON_Delete(root);
    return ServerMessageResult::kContinue;
}

std::string GeminiTranscribeClient::BuildSetupMessage(std::string& error) const {
    std::vector<std::string> vocabulary;
    if (!ParseVocabulary(config_.gemini_vocabulary, vocabulary, error)) {
        return {};
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* setup = root != nullptr ? cJSON_AddObjectToObject(root, "setup") : nullptr;
    cJSON* generation =
        setup != nullptr ? cJSON_AddObjectToObject(setup, "generationConfig") : nullptr;
    cJSON* modalities =
        generation != nullptr ? cJSON_AddArrayToObject(generation, "responseModalities") : nullptr;
    cJSON* transcription =
        setup != nullptr ? cJSON_AddObjectToObject(setup, "inputAudioTranscription") : nullptr;
    cJSON* languages =
        transcription != nullptr ? cJSON_AddArrayToObject(transcription, "languageCodes") : nullptr;
    cJSON* text_modality = cJSON_CreateString("TEXT");
    cJSON* language = cJSON_CreateString(config_.gemini_language.c_str());

    if (root == nullptr || setup == nullptr || generation == nullptr || modalities == nullptr ||
        transcription == nullptr || languages == nullptr || text_modality == nullptr ||
        language == nullptr || cJSON_AddStringToObject(setup, "model", kModel) == nullptr ||
        cJSON_AddStringToObject(transcription, "mode", GeminiAsrModeName(config_.gemini_mode)) ==
            nullptr) {
        cJSON_Delete(text_modality);
        cJSON_Delete(language);
        cJSON_Delete(root);
        error = "Out of memory while building Gemini setup";
        return {};
    }
    if (cJSON_AddItemToArray(modalities, text_modality) == 0) {
        cJSON_Delete(text_modality);
        cJSON_Delete(language);
        cJSON_Delete(root);
        error = "Out of memory while building Gemini setup";
        return {};
    }
    if (cJSON_AddItemToArray(languages, language) == 0) {
        cJSON_Delete(language);
        cJSON_Delete(root);
        error = "Out of memory while building Gemini setup";
        return {};
    }

    if (!vocabulary.empty()) {
        cJSON* custom = cJSON_AddArrayToObject(transcription, "customVocabulary");
        if (custom == nullptr) {
            cJSON_Delete(root);
            error = "Out of memory while building Gemini vocabulary";
            return {};
        }
        for (const auto& term : vocabulary) {
            cJSON* item = cJSON_CreateString(term.c_str());
            if (item == nullptr || cJSON_AddItemToArray(custom, item) == 0) {
                cJSON_Delete(item);
                cJSON_Delete(root);
                error = "Out of memory while building Gemini vocabulary";
                return {};
            }
        }
    }

    char* encoded = cJSON_PrintUnformatted(root);
    std::string result = encoded != nullptr ? encoded : "";
    cJSON_free(encoded);
    cJSON_Delete(root);
    if (result.empty()) {
        error = "Out of memory while serializing Gemini setup";
    }
    return result;
}

void GeminiTranscribeClient::CloseSocket() {
    if (websocket_ != nullptr) {
        websocket_->Close();
        websocket_.reset();
    }
}
