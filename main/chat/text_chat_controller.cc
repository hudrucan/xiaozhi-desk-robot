#include "chat/text_chat_controller.h"

#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <utility>

#define TAG "TextChat"

namespace {

constexpr int64_t kTimeoutUs = 60LL * 1000 * 1000;
constexpr int64_t kTtsStopGraceUs = 750LL * 1000;
constexpr int64_t kAudioQuietGraceUs = 300LL * 1000;

// Official MQTT validates each listen/detect payload independently, including
// while a conversation is already active. Keep native detect/text only for
// very short typed messages; longer Web UI messages use the MCP bridge.
constexpr size_t kNativeDetectMaxCodepoints = 12;
constexpr char kMcpTrigger[] = "web_chat";
constexpr char kMcpToolName[] = "self.web_chat.consume_pending";

size_t CountUtf8Codepoints(const std::string& value) {
    size_t count = 0;
    for (unsigned char c : value) {
        if ((c & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TextChatController::TextChatController(Application& application) : application_(application) {}

void TextChatController::WebChatMcpBridge::Arm(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_text_ = text;
    display_text_ = text;
}

void TextChatController::WebChatMcpBridge::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_text_.clear();
    display_text_.clear();
}

std::string TextChatController::WebChatMcpBridge::ConsumePending() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text = std::move(pending_text_);
    pending_text_.clear();
    return text;
}

std::string TextChatController::WebChatMcpBridge::GetDisplayText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return display_text_;
}

void TextChatController::RegisterMcpTool(McpServer& mcp_server) {
    mcp_server.AddPriorityAssistantOnlyTool(
        kMcpToolName,
        "When detect text is exactly 'web_chat', call this tool immediately before responding. "
        "It returns the full user message; answer that message, never the literal trigger.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            auto pending = web_chat_bridge_.ConsumePending();
            if (pending.empty()) {
                return std::unexpected("No pending Web UI message");
            }

            ESP_LOGI(TAG, "MCP bridge consumed chars=%u bytes=%u",
                     static_cast<unsigned>(CountUtf8Codepoints(pending)),
                     static_cast<unsigned>(pending.size()));
            return pending;
        });
}

bool TextChatController::Submit(std::string text, bool require_active_conversation,
                                std::string& message) {
    if (text.empty()) {
        message = "Text chat input is empty";
        return false;
    }
    const auto state = application_.GetDeviceState();
    if (require_active_conversation &&
        (state != kDeviceStateListening || !application_.protocol_ ||
         !application_.protocol_->IsAudioChannelOpened() ||
         application_.protocol_->session_id().empty())) {
        message = "Active conversation is not available";
        return false;
    }
    if (!require_active_conversation && state != kDeviceStateIdle &&
        state != kDeviceStateListening) {
        message = "Text chat requires Idle or Listening";
        return false;
    }
    bool expected = false;
    if (!pending_.compare_exchange_strong(expected, true)) {
        message = "Text chat is already running";
        return false;
    }

    ResetActiveState();
    if (require_active_conversation) {
        Emit("user", text);
    }

    const size_t codepoints = CountUtf8Codepoints(text);
    if (codepoints > kNativeDetectMaxCodepoints) {
        web_chat_bridge_.Arm(text);
        ESP_LOGI(TAG, "MCP bridge armed chars=%u bytes=%u trigger=%s",
                 static_cast<unsigned>(codepoints), static_cast<unsigned>(text.size()),
                 kMcpTrigger);
        application_.Schedule([this]() { Run(kMcpTrigger); });
    } else {
        web_chat_bridge_.Clear();
        application_.Schedule([this, text = std::move(text)]() { Run(text); });
    }

    message = "Text chat queued";
    return true;
}

void TextChatController::RegisterCallback(
    std::function<void(const std::string&, const std::string&)> callback) {
    callback_ = std::move(callback);
}

void TextChatController::Emit(const std::string& event, const std::string& text) {
    if (callback_) {
        callback_(event, text);
    }
}

void TextChatController::ResetActiveState() {
    deadline_us_.store(0);
    tts_active_.store(false);
    tts_stopped_.store(false);
    assistant_started_.store(false);
    audio_packets_.store(0);
    last_audio_us_.store(0);
    tts_stop_us_.store(0);
}

void TextChatController::HandleClockTick() {
    if (tts_stopped_.load()) {
        CompleteAfterPlayback();
    }

    const int64_t deadline = deadline_us_.load();
    if (!pending_.load() || deadline <= 0 || esp_timer_get_time() < deadline ||
        !pending_.exchange(false)) {
        return;
    }

    ResetActiveState();
    web_chat_bridge_.Clear();
    ESP_LOGE(TAG, "Rejected reason=timeout");
    Emit("error", "Protocol timeout: no TTS response");
    if (application_.protocol_ && application_.protocol_->IsAudioChannelOpened()) {
        if (resume_listening_) {
            ResumeListening();
        } else if (application_.GetDeviceState() == kDeviceStateListening) {
            application_.SetDeviceState(kDeviceStateIdle);
        }
    }
}

void TextChatController::HandlePlaybackDrained() {
    CompleteAfterPlayback();
}

bool TextChatController::Fail(const std::string& detail, bool restore_state) {
    if (!pending_.exchange(false)) {
        return false;
    }

    ResetActiveState();
    ESP_LOGE(TAG, "Rejected reason=%s", detail.c_str());
    Emit("error", detail);
    web_chat_bridge_.Clear();

    if (restore_state) {
        application_.Schedule([this]() {
            if (resume_listening_) {
                ResumeListening();
            } else if (application_.GetDeviceState() == kDeviceStateListening) {
                application_.SetDeviceState(kDeviceStateIdle);
            }
        });
    }
    return true;
}

void TextChatController::RecordIncomingAudio() {
    if (pending_.load() || tts_active_.load()) {
        audio_packets_.fetch_add(1);
        last_audio_us_.store(esp_timer_get_time());
    }
}

void TextChatController::OnAudioChannelClosed() {
    resume_listening_ = false;
}

void TextChatController::OnTtsStart() {
    if (pending_.load()) {
        tts_active_.store(true);
        tts_stopped_.store(false);
        ESP_LOGI(TAG, "TTS started");
    }
    Emit("speaking");
}

bool TextChatController::OnTtsStop() {
    if (!pending_.load()) {
        Emit("completed");
        return false;
    }

    deadline_us_.store(0);
    tts_stopped_.store(true);
    tts_stop_us_.store(esp_timer_get_time());
    ESP_LOGI(TAG, "tts/stop audio_packets=%lu",
             static_cast<unsigned long>(audio_packets_.load()));
    application_.Schedule([this]() { CompleteAfterPlayback(); });
    return true;
}

void TextChatController::OnAssistantText(const std::string& text) {
    if (pending_.load()) {
        if (!assistant_started_.exchange(true)) {
            ESP_LOGI(TAG, "Assistant started");
        }
        ESP_LOGI(TAG, "tts/sentence_start text=%s", text.c_str());
    }
    Emit("assistant", text);
}

std::string TextChatController::ResolveIncomingTranscript(const std::string& text) {
    if (!pending_.load()) {
        Emit("user", text);
        return text;
    }

    ESP_LOGI(TAG, "Incoming STT text=%s", text.c_str());
    if (text != kMcpTrigger) {
        return text;
    }
    auto original = web_chat_bridge_.GetDisplayText();
    if (original.empty()) {
        return text;
    }
    ESP_LOGI(TAG, "MCP bridge substituted trigger with original text");
    return original;
}

void TextChatController::OnMcpMessage() const {
    if (pending_.load()) {
        ESP_LOGI(TAG, "MCP message");
    }
}

void TextChatController::CompleteAfterPlayback() {
    if (!pending_.load() || !tts_stopped_.load()) {
        return;
    }

    // TTS control JSON and audio packets may use different transports. Keep
    // accepting late audio briefly when stop arrives before the final Opus data.
    const int64_t now = esp_timer_get_time();
    const int64_t stop_time = tts_stop_us_.load();
    const int64_t last_audio_time = last_audio_us_.load();
    if (stop_time == 0 || now - stop_time < kTtsStopGraceUs ||
        (last_audio_time > 0 && now - last_audio_time < kAudioQuietGraceUs) ||
        !application_.audio_service_.IsPlaybackIdle()) {
        return;
    }
    if (!pending_.exchange(false)) {
        return;
    }

    deadline_us_.store(0);
    tts_active_.store(false);
    tts_stopped_.store(false);
    tts_stop_us_.store(0);
    ESP_LOGI(TAG, "Completed audio_packets=%lu",
             static_cast<unsigned long>(audio_packets_.load()));
    Emit("completed");
    web_chat_bridge_.Clear();
    if (resume_listening_) {
        ResumeListening();
    } else if (application_.GetDeviceState() == kDeviceStateSpeaking) {
        application_.listening_mode_ = application_.GetDefaultListeningMode();
        application_.SetDeviceState(kDeviceStateListening);
    }
}

void TextChatController::ResumeListening() {
    if (!resume_listening_) {
        return;
    }
    resume_listening_ = false;
    if (!application_.protocol_ || !application_.protocol_->IsAudioChannelOpened()) {
        return;
    }
    application_.listening_mode_ = resume_mode_;
    if (application_.GetDeviceState() == kDeviceStateListening) {
        application_.StartListeningAudio();
    } else {
        application_.SetDeviceState(kDeviceStateListening);
    }
}

void TextChatController::Run(const std::string& text) {
    if (!pending_.load()) {
        return;
    }

    const auto reject = [this](const std::string& detail, bool close_channel = false,
                               bool return_idle = false) {
        pending_.store(false);
        ResetActiveState();
        ESP_LOGE(TAG, "Rejected reason=%s", detail.c_str());
        Emit("error", detail);
        web_chat_bridge_.Clear();

        if (close_channel && application_.protocol_ &&
            application_.protocol_->IsAudioChannelOpened()) {
            application_.protocol_->CloseAudioChannel();
        }
        if (return_idle && application_.GetDeviceState() != kDeviceStateIdle) {
            application_.SetDeviceState(kDeviceStateIdle);
        }
    };

    const auto state = application_.GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateListening) {
        reject("Robot is no longer Idle or Listening");
        return;
    }
    if (!application_.protocol_) {
        reject("Protocol is not initialized");
        return;
    }

    resume_listening_ = state == kDeviceStateListening;
    resume_mode_ = application_.listening_mode_;
    if (resume_listening_) {
        application_.pending_listening_start_ = false;
        if (application_.active_asr_provider_ == AsrProvider::kGemini) {
            application_.StopGeminiAsrTurn();
            application_.ResetAsrTurnConfig();
        }
        application_.asr_ready_.store(false);
        application_.audio_service_.EnableVoiceProcessing(false);
    }

    while (application_.audio_service_.PopPacketFromSendQueue()) {
    }

    if (!application_.protocol_->IsAudioChannelOpened()) {
        if (!application_.SetDeviceState(kDeviceStateConnecting)) {
            reject("Could not enter connecting state");
            return;
        }
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (!application_.protocol_->OpenAudioChannel()) {
            reject("Could not open existing protocol channel", false, true);
            return;
        }
    }

    if (application_.protocol_->session_id().empty()) {
        reject("Session is not ready", true, true);
        return;
    }

    application_.audio_service_.ResetDecoder();
    deadline_us_.store(esp_timer_get_time() + kTimeoutUs);

    if (!resume_listening_) {
        application_.listening_mode_ = application_.GetDefaultListeningMode();
        application_.pending_listening_start_ = false;
        application_.audio_service_.EnableVoiceProcessing(false);
        application_.audio_service_.EnableWakeWordDetection(false);

        if (!application_.SetDeviceState(kDeviceStateListening)) {
            reject("Could not enter listening state", true, true);
            return;
        }

        // Bind the MQTT session's UDP return path with deterministic valid Opus
        // silence; never use microphone audio for an Idle-origin typed turn.
        application_.protocol_->SendStartListening(application_.listening_mode_);
        if (!application_.protocol_->PrimeAudioChannel()) {
            reject("UDP audio prime failed", true, true);
            return;
        }

        application_.audio_service_.EnableVoiceProcessing(false);
        application_.audio_service_.EnableWakeWordDetection(false);
        while (application_.audio_service_.PopPacketFromSendQueue()) {
        }
        ESP_LOGI(TAG, "Idle-origin: UDP silence prime sent, mic suppressed");
    }

    ESP_LOGI(TAG, "Sending detect/text");
    if (!application_.protocol_->SendWakeWordDetected(text)) {
        reject("Send failed", true, !resume_listening_);
        return;
    }

    ESP_LOGI(TAG, "Sent");
    Emit("sent");
}
