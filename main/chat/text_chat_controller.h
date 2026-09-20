#ifndef TEXT_CHAT_CONTROLLER_H_
#define TEXT_CHAT_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "protocol.h"

class Application;
class McpServer;

class TextChatController {
public:
    explicit TextChatController(Application& application);

    void RegisterMcpTool(McpServer& mcp_server);
    void SetEnhancedTypedTextEnabled(bool enabled) {
        enhanced_typed_text_enabled_.store(enabled);
    }
    bool Submit(std::string text, bool require_active_conversation, std::string& message);
    void RegisterCallback(
        std::function<void(const std::string&, const std::string&)> callback);

    bool IsPending() const { return pending_.load(); }
    bool IsTtsActive() const { return tts_active_.load(); }
    bool ShouldResumeListening() const { return resume_listening_; }
    bool IsIdleOriginPending() const { return pending_.load() && !resume_listening_; }

    void HandleClockTick();
    void HandlePlaybackDrained();
    bool Fail(const std::string& detail, bool restore_state);
    void RecordIncomingAudio();
    void OnAudioChannelClosed();
    void OnTtsStart();
    bool OnTtsStop();
    void OnAssistantText(const std::string& text);
    std::string ResolveIncomingTranscript(const std::string& text);
    void OnMcpMessage();

private:
    class WebChatMcpBridge {
    public:
        void Arm(const std::string& text);
        void Clear();
        std::string ConsumePending();
        std::string GetDisplayText() const;

    private:
        mutable std::mutex mutex_;
        std::string pending_text_;
        std::string display_text_;
    };

    Application& application_;
    WebChatMcpBridge web_chat_bridge_;
    std::atomic_bool enhanced_typed_text_enabled_{false};
    std::atomic_bool pending_{false};
    std::atomic<int64_t> deadline_us_{0};
    std::atomic_bool tts_active_{false};
    std::atomic_bool tts_stopped_{false};
    std::atomic_bool assistant_started_{false};
    std::atomic<uint32_t> audio_packets_{0};
    std::atomic<int64_t> last_audio_us_{0};
    std::atomic<int64_t> tts_stop_us_{0};
    std::function<void(const std::string&, const std::string&)> callback_;
    bool resume_listening_ = false;
    ListeningMode resume_mode_ = kListeningModeAutoStop;

    void Run(const std::string& text, bool enhanced_typed_text);
    void Emit(const std::string& event, const std::string& text = "");
    void CompleteAfterPlayback();
    void ResumeListening();
    void ResetActiveState();
    void RefreshDeadline();
};

#endif  // TEXT_CHAT_CONTROLLER_H_
