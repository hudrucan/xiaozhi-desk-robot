#ifndef GEMINI_TRANSCRIBE_CLIENT_H
#define GEMINI_TRANSCRIBE_CLIENT_H

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "asr_settings.h"
#include "fixed_queue.h"

class WebSocket;

class GeminiTranscribeClient {
public:
    enum class State {
        kIdle,
        kConnecting,
        kConnected,
        kAwaitingSetup,
        kReady,
        kStreaming,
        kEnding,
        kFinalized,
        kClosed,
        kError,
    };

    struct Callbacks {
        // Callbacks run on the Gemini worker task, never on an audio task.
        // Application mutations must be marshalled back to the application context.
        // The owner must keep this client alive until IsRunning() becomes false.
        std::function<void(State)> on_state;
        std::function<void(const std::string&)> on_interim_transcript;
        std::function<void(const std::string&)> on_final_transcript;
        std::function<void(const std::string&)> on_error;
    };

    GeminiTranscribeClient();
    ~GeminiTranscribeClient();

    GeminiTranscribeClient(const GeminiTranscribeClient&) = delete;
    GeminiTranscribeClient& operator=(const GeminiTranscribeClient&) = delete;

    bool Start(const AsrConfig& config, Callbacks callbacks);

    // Non-blocking and accepted only after setupComplete (kReady/kStreaming).
    // When full, the oldest queued realtime frame is discarded; lock contention
    // drops the incoming frame instead of waiting on the audio task.
    bool PushPcm(std::vector<int16_t>&& pcm);
    bool SendAudioStreamEnd();

    // Non-blocking cancellation request. The worker owns all socket teardown.
    void Cancel();

    State state() const { return state_.load(); }
    bool IsRunning() const { return worker_running_.load(); }
    uint32_t pcm_drop_count() const { return pcm_drop_count_.load(); }
    uint32_t connect_latency_ms() const { return connect_latency_ms_.load(); }
    std::string GetLastError() const;

    static const char* StateName(State state);

private:
    static constexpr EventBits_t kWorkerStopped = 1 << 0;
    static constexpr size_t kPcmQueueCapacity = 6;
    static constexpr size_t kMessageQueueCapacity = 8;
    static constexpr size_t kMaxServerMessageBytes = 16 * 1024;
    static constexpr size_t kSendChunkSamples = 1600;  // 100 ms at 16 kHz.
    static constexpr size_t kMaxPcmFrameSamples = 3200;
    static constexpr int64_t kSetupTimeoutUs = 10LL * 1000 * 1000;

    enum class ServerMessageResult {
        kContinue,
        kFinalReceived,
        kError,
    };

    struct ServerMessage {
        std::string payload;
        bool binary = false;
    };

    std::atomic<State> state_{State::kIdle};
    std::atomic<bool> worker_running_{false};
    std::atomic<bool> worker_created_{false};
    std::atomic<bool> first_server_message_logged_{false};
    std::atomic<uint32_t> pcm_drop_count_{0};
    std::atomic<uint32_t> connect_latency_ms_{0};

    EventGroupHandle_t worker_events_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    FixedQueue<std::vector<int16_t>, kPcmQueueCapacity> pcm_queue_;
    FixedQueue<ServerMessage, kMessageQueueCapacity> message_queue_;
    AsrConfig config_;
    Callbacks callbacks_;
    std::unique_ptr<WebSocket> websocket_;
    std::string last_error_;
    std::string network_error_;
    bool cancel_requested_ = false;
    bool audio_stream_end_requested_ = false;
    bool remote_disconnected_ = false;
    bool setup_complete_ = false;
    bool final_callback_sent_ = false;

    static void WorkerTask(void* argument);
    void WorkerLoop();
    void QueueServerMessage(const char* data, size_t length, bool binary);
    void QueueNetworkError(const std::string& error);
    void SetState(State state);
    void Fail(const std::string& error);
    bool IsCancelRequested() const;
    bool SendPcmChunk(const int16_t* samples, size_t sample_count);
    ServerMessageResult HandleServerMessage(const ServerMessage& message);
    std::string BuildSetupMessage(std::string& error) const;
    void CloseSocket();
};

#endif  // GEMINI_TRANSCRIBE_CLIENT_H
