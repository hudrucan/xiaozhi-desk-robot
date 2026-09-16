#ifndef GEMINI_ASR_TURN_CONTROLLER_H_
#define GEMINI_ASR_TURN_CONTROLLER_H_

#include <atomic>
#include <cstdint>
#include <string>

#include "asr_settings.h"
#include "device_state.h"
#include "gemini_transcribe_client.h"

class Application;

class GeminiAsrTurnController {
public:
    explicit GeminiAsrTurnController(Application& application);

    bool IsReady() const { return ready_.load(); }
    bool IsPreparing() const { return preparing_.load(); }
    bool IsGeminiActive() const { return active_provider_ == AsrProvider::kGemini; }
    bool HasTurnConfig() const { return turn_config_valid_; }

    const AsrConfig& GetTurnConfig();
    void ResetTurnConfig();
    bool PrimeAudioChannel();
    void StartListeningAudio();
    void MaybeStartPrewarm();
    void Stop();

    void ObserveVad(bool speaking);
    void HandleVadChange();
    void HandleTimers();
    void HandleApplicationStateChanged(DeviceState new_state);
    void SetPreparingForListening();
    void SetNotReady() { ready_.store(false); }

private:
    Application& application_;
    GeminiTranscribeClient client_;
    AsrConfig turn_config_;
    bool turn_config_valid_ = false;
    AsrProvider active_provider_ = AsrProvider::kXiaozhi;
    uint32_t turn_id_ = 0;
    std::atomic_bool vad_turn_active_{false};
    std::atomic_bool vad_speech_started_{false};
    std::atomic_bool vad_end_pending_{false};
    bool vad_speech_start_handled_ = false;
    bool audio_stream_end_requested_ = false;
    bool restart_pending_ = false;
    bool prewarm_retry_pending_ = false;
    std::atomic_bool prewarming_{false};
    int64_t prewarm_started_us_ = 0;
    int64_t listening_deadline_us_ = 0;
    std::atomic_bool ready_{false};
    std::atomic_bool preparing_{false};

    void StartTurn(const AsrConfig& config);
    void StartClient(const AsrConfig& config, bool prewarming);
    void HandleReady(uint32_t turn_id);
    void HandleFinal(uint32_t turn_id, std::string transcript);
    void HandleFinalTimeout(uint32_t turn_id);
    void HandleError(uint32_t turn_id, std::string error, bool failed_during_prewarm);
    void HandleWorkerStopped();
    void RecoverTurn(uint32_t turn_id, const char* reason);
};

#endif  // GEMINI_ASR_TURN_CONTROLLER_H_
