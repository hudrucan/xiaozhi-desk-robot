#include "audio/gemini_asr_turn_controller.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <utility>

#define TAG "GeminiASRTurn"

GeminiAsrTurnController::GeminiAsrTurnController(Application& application)
    : application_(application) {}

const AsrConfig& GeminiAsrTurnController::GetTurnConfig() {
    if (!turn_config_valid_) {
        turn_config_ = AsrSettings::Load();
        turn_config_valid_ = true;
    }
    return turn_config_;
}

void GeminiAsrTurnController::ResetTurnConfig() {
    std::fill(turn_config_.gemini_api_key.begin(), turn_config_.gemini_api_key.end(), '\0');
    turn_config_ = AsrConfig{};
    turn_config_valid_ = false;
}

bool GeminiAsrTurnController::PrimeAudioChannel() {
    if (GetTurnConfig().provider != AsrProvider::kGemini) {
        return true;
    }
    if (application_.protocol_ && application_.protocol_->PrimeAudioChannel()) {
        return true;
    }

    application_.last_error_message_ = "Could not prime conversation audio channel";
    xEventGroupSetBits(application_.event_group_, MAIN_EVENT_ERROR);
    return false;
}

void GeminiAsrTurnController::StartListeningAudio() {
    if (application_.GetDeviceState() != kDeviceStateListening) {
        return;
    }

    const AsrConfig& config = GetTurnConfig();
    ESP_LOGI(TAG, "ASR turn start provider=%s configured=%d", AsrProviderName(config.provider),
             config.IsGeminiConfigured() ? 1 : 0);
    if (config.provider == AsrProvider::kGemini) {
        StartTurn(config);
        return;
    }

    // Preserve the existing Xiaozhi listening path.
    if (IsGeminiActive()) {
        Stop();
    }
    active_provider_ = AsrProvider::kXiaozhi;
    application_.protocol_->SendStartListening(application_.listening_mode_);
    application_.audio_service_.SetAsrProvider(AsrProvider::kXiaozhi, nullptr);
    application_.audio_service_.EnableVoiceProcessing(true);
    preparing_.store(false);
    ready_.store(true);

    application_.ConfigureWakeWordForListening();
    if (application_.play_popup_on_listening_) {
        application_.play_popup_on_listening_ = false;
        application_.audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void GeminiAsrTurnController::StartTurn(const AsrConfig& config) {
    if (IsGeminiActive()) {
        if (!prewarming_.load()) {
            return;
        }

        const int64_t elapsed_us =
            prewarm_started_us_ > 0 ? esp_timer_get_time() - prewarm_started_us_ : 0;
        prewarming_ = false;
        prewarm_started_us_ = 0;
        ESP_LOGI(TAG,
                 "Prewarm promoted state=%s elapsed_ms=%lld free_internal=%u min_internal=%u",
                 GeminiTranscribeClient::StateName(client_.state()),
                 static_cast<long long>(elapsed_us / 1000),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));

        const auto client_state = client_.state();
        if (client_state == GeminiTranscribeClient::State::kReady ||
            client_state == GeminiTranscribeClient::State::kStreaming) {
            HandleReady(turn_id_);
        } else if (!client_.IsRunning()) {
            Stop();
            StartClient(config, false);
        }
        return;
    }

    if (client_.IsRunning()) {
        restart_pending_ = true;
        ESP_LOGI(TAG, "Waiting for previous worker before cold start");
        return;
    }
    StartClient(config, false);
}

void GeminiAsrTurnController::MaybeStartPrewarm() {
    if (application_.GetDeviceState() != kDeviceStateSpeaking || !application_.protocol_ ||
        !application_.protocol_->IsAudioChannelOpened() || IsGeminiActive()) {
        prewarm_retry_pending_ = false;
        return;
    }

    const bool will_resume_listening =
        application_.listening_mode_ != kListeningModeManualStop ||
        application_.text_chat_controller_.ShouldResumeListening();
    if (!will_resume_listening) {
        prewarm_retry_pending_ = false;
        return;
    }

    const AsrConfig& config = GetTurnConfig();
    if (config.provider != AsrProvider::kGemini || !config.IsGeminiConfigured()) {
        prewarm_retry_pending_ = false;
        return;
    }

    if (client_.IsRunning()) {
        if (!prewarm_retry_pending_) {
            ESP_LOGI(TAG, "Prewarm deferred; previous worker is stopping");
        }
        prewarm_retry_pending_ = true;
        return;
    }

    prewarm_retry_pending_ = false;
    ESP_LOGI(TAG, "Prewarm starting free_internal=%u min_internal=%u largest_internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    StartClient(config, true);
}

void GeminiAsrTurnController::StartClient(const AsrConfig& config, bool prewarming) {
    if (IsGeminiActive() || client_.IsRunning()) {
        return;
    }

    // Keep processed PCM away from both ASR providers while connect/setup runs
    // on GeminiTranscribeClient's worker task.
    if (!prewarming) {
        application_.audio_service_.EnableVoiceProcessing(false);
    }
    application_.audio_service_.SetAsrProvider(AsrProvider::kGemini, nullptr);
    if (!prewarming) {
        application_.audio_service_.EnableWakeWordDetection(false);
        Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PREPARING_ASR);
    }
    ready_.store(false);
    preparing_.store(!prewarming);
    vad_turn_active_.store(false);
    vad_speech_started_.store(false);
    vad_end_pending_.store(false);
    vad_speech_start_handled_ = false;
    audio_stream_end_requested_ = false;
    restart_pending_ = false;
    prewarming_.store(prewarming);
    prewarm_started_us_ = prewarming ? esp_timer_get_time() : 0;
    listening_deadline_us_ = 0;

    active_provider_ = AsrProvider::kGemini;
    uint32_t turn_id = ++turn_id_;
    if (turn_id == 0) {
        turn_id = ++turn_id_;
    }

    GeminiTranscribeClient::Callbacks callbacks;
    callbacks.on_state = [this, turn_id](GeminiTranscribeClient::State state) {
        if (state == GeminiTranscribeClient::State::kReady) {
            application_.Schedule([this, turn_id]() { HandleReady(turn_id); });
        }
    };
    callbacks.on_final_transcript = [this, turn_id](const std::string& transcript) {
        application_.Schedule(
            [this, turn_id, transcript]() mutable { HandleFinal(turn_id, std::move(transcript)); });
    };
    callbacks.on_final_timeout = [this, turn_id]() {
        application_.Schedule([this, turn_id]() { HandleFinalTimeout(turn_id); });
    };
    callbacks.on_error = [this, turn_id](const std::string& error) {
        const bool failed_during_prewarm = prewarming_.load();
        application_.Schedule([this, turn_id, error, failed_during_prewarm]() {
            HandleError(turn_id, error, failed_during_prewarm);
        });
    };
    callbacks.on_stopped = [this]() {
        application_.Schedule([this]() { HandleWorkerStopped(); });
    };

    if (!client_.Start(config, std::move(callbacks))) {
        HandleError(turn_id, client_.GetLastError(), prewarming);
    }
}

void GeminiAsrTurnController::HandleReady(uint32_t turn_id) {
    if (turn_id != turn_id_ || !IsGeminiActive()) {
        return;
    }
    const auto state = client_.state();
    if (state != GeminiTranscribeClient::State::kReady &&
        state != GeminiTranscribeClient::State::kStreaming) {
        return;
    }

    if (prewarming_.load()) {
        const int64_t elapsed_us =
            prewarm_started_us_ > 0 ? esp_timer_get_time() - prewarm_started_us_ : 0;
        ESP_LOGI(TAG,
                 "Prewarm ready total_ms=%lld connect_ms=%lu free_internal=%u min_internal=%u",
                 static_cast<long long>(elapsed_us / 1000),
                 static_cast<unsigned long>(client_.connect_latency_ms()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
        return;
    }
    if (application_.GetDeviceState() != kDeviceStateListening || ready_.load()) {
        return;
    }

    application_.play_popup_on_listening_ = false;
    vad_speech_started_.store(false);
    vad_end_pending_.store(false);
    vad_speech_start_handled_ = false;
    audio_stream_end_requested_ = false;
    vad_turn_active_.store(true);
    application_.audio_service_.SetAsrProvider(AsrProvider::kGemini, &client_);
    application_.audio_service_.EnableVoiceProcessing(true);
    application_.ConfigureWakeWordForListening();
    ready_.store(true);
    preparing_.store(false);
    Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::LISTENING);
    listening_deadline_us_ =
        esp_timer_get_time() +
        static_cast<int64_t>(Protocol::kChannelInactivityTimeoutSeconds) * 1000 * 1000;
    ESP_LOGI(TAG, "Ready; voice processing enabled");
}

void GeminiAsrTurnController::ObserveVad(bool speaking) {
    if (!vad_turn_active_.load()) {
        return;
    }
    if (speaking) {
        vad_speech_started_.store(true);
    } else if (vad_speech_started_.load()) {
        vad_end_pending_.store(true);
    }
}

void GeminiAsrTurnController::HandleVadChange() {
    if (vad_turn_active_.load() && vad_speech_started_.load() &&
        !vad_speech_start_handled_) {
        vad_speech_start_handled_ = true;
        listening_deadline_us_ = 0;
        ESP_LOGI(TAG, "Speech started turn=%lu", static_cast<unsigned long>(turn_id_));
    }

    if (!vad_end_pending_.exchange(false) || !vad_turn_active_.exchange(false) ||
        !IsGeminiActive() || application_.GetDeviceState() != kDeviceStateListening ||
        audio_stream_end_requested_) {
        return;
    }

    ESP_LOGI(TAG, "VAD end turn=%lu", static_cast<unsigned long>(turn_id_));
    ready_.store(false);
    Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PROCESSING);
    application_.audio_service_.EnableVoiceProcessing(false);
    application_.audio_service_.SetAsrProvider(AsrProvider::kGemini, nullptr);
    audio_stream_end_requested_ = true;

    if (!client_.SendAudioStreamEnd()) {
        const auto state = client_.state();
        if (state != GeminiTranscribeClient::State::kEnding &&
            state != GeminiTranscribeClient::State::kFinalized &&
            state != GeminiTranscribeClient::State::kClosed) {
            ESP_LOGW(TAG, "audioStreamEnd request ignored in state=%s",
                     GeminiTranscribeClient::StateName(state));
        }
    } else {
        ESP_LOGI(TAG, "audioStreamEnd requested turn=%lu", static_cast<unsigned long>(turn_id_));
    }
}

void GeminiAsrTurnController::HandleFinal(uint32_t turn_id, std::string transcript) {
    if (turn_id != turn_id_ || !IsGeminiActive()) {
        return;
    }
    if (transcript.empty()) {
        RecoverTurn(turn_id, "empty finalized transcript");
        return;
    }

    ready_.store(false);
    Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PROCESSING);
    ESP_LOGI(TAG, "Final received bytes=%u", static_cast<unsigned>(transcript.size()));
    Stop();
    ResetTurnConfig();

    std::string message;
    if (!application_.text_chat_controller_.Submit(std::move(transcript), true, message)) {
        if (application_.text_chat_controller_.IsPending()) {
            ESP_LOGW(TAG, "Final superseded by another text turn");
            return;
        }
        application_.last_error_message_ = "Gemini ASR: " + message;
        xEventGroupSetBits(application_.event_group_, MAIN_EVENT_ERROR);
    }
}

void GeminiAsrTurnController::HandleFinalTimeout(uint32_t turn_id) {
    RecoverTurn(turn_id, "final transcript timeout");
}

void GeminiAsrTurnController::RecoverTurn(uint32_t turn_id, const char* reason) {
    if (turn_id != turn_id_ || !IsGeminiActive()) {
        return;
    }
    ESP_LOGW(TAG, "Utterance discarded turn=%lu reason=%s", static_cast<unsigned long>(turn_id),
             reason);
    Stop();
    ResetTurnConfig();
    if (application_.GetDeviceState() == kDeviceStateListening && application_.protocol_ &&
        application_.protocol_->IsAudioChannelOpened()) {
        restart_pending_ = true;
        preparing_.store(true);
        Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PREPARING_ASR);
    }
}

void GeminiAsrTurnController::HandleTimers() {
    if (restart_pending_) {
        if (application_.GetDeviceState() != kDeviceStateListening || !application_.protocol_ ||
            !application_.protocol_->IsAudioChannelOpened()) {
            restart_pending_ = false;
        } else if (!client_.IsRunning()) {
            restart_pending_ = false;
            StartListeningAudio();
        }
    }

    if (listening_deadline_us_ <= 0 || esp_timer_get_time() < listening_deadline_us_) {
        return;
    }
    listening_deadline_us_ = 0;
    if (application_.GetDeviceState() != kDeviceStateListening || !IsGeminiActive() ||
        vad_speech_started_.load()) {
        return;
    }

    ESP_LOGI(TAG, "Listening inactivity timeout=%ds; ending conversation",
             Protocol::kChannelInactivityTimeoutSeconds);
    Stop();
    ResetTurnConfig();
    if (application_.protocol_) {
        application_.protocol_->CloseAudioChannel();
    }
    if (application_.GetDeviceState() == kDeviceStateListening) {
        application_.SetDeviceState(kDeviceStateIdle);
    }
}

void GeminiAsrTurnController::HandleError(uint32_t turn_id, std::string error,
                                          bool failed_during_prewarm) {
    if (turn_id != turn_id_ || !IsGeminiActive()) {
        return;
    }
    if (failed_during_prewarm) {
        ESP_LOGW(TAG, "Prewarm failed; cold start will be used: %s", error.c_str());
        Stop();
        ResetTurnConfig();
        if (application_.GetDeviceState() == kDeviceStateListening && application_.protocol_ &&
            application_.protocol_->IsAudioChannelOpened()) {
            preparing_.store(true);
            Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PREPARING_ASR);
            if (!application_.pending_listening_start_) {
                restart_pending_ = true;
            }
        }
        return;
    }

    Stop();
    application_.last_error_message_ =
        error.empty() ? "Gemini ASR failed" : "Gemini ASR: " + error;
    xEventGroupSetBits(application_.event_group_, MAIN_EVENT_ERROR);
}

void GeminiAsrTurnController::HandleWorkerStopped() {
    if (client_.IsRunning()) {
        return;
    }
    if (restart_pending_) {
        if (application_.GetDeviceState() == kDeviceStateListening && application_.protocol_ &&
            application_.protocol_->IsAudioChannelOpened()) {
            restart_pending_ = false;
            ESP_LOGI(TAG, "Worker stopped; resuming pending listening start");
            StartListeningAudio();
            return;
        }
        if (application_.GetDeviceState() != kDeviceStateListening) {
            restart_pending_ = false;
        }
    }

    if (!prewarm_retry_pending_) {
        return;
    }
    prewarm_retry_pending_ = false;
    if (application_.GetDeviceState() == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Worker stopped; starting deferred prewarm");
        MaybeStartPrewarm();
    }
}

void GeminiAsrTurnController::Stop() {
    if (!IsGeminiActive()) {
        return;
    }
    const bool was_prewarming = prewarming_.load();
    vad_turn_active_.store(false);
    vad_speech_started_.store(false);
    vad_end_pending_.store(false);
    vad_speech_start_handled_ = false;
    audio_stream_end_requested_ = false;
    restart_pending_ = false;
    prewarming_ = false;
    prewarm_started_us_ = 0;
    listening_deadline_us_ = 0;
    ready_.store(false);
    preparing_.store(false);
    if (!was_prewarming) {
        application_.audio_service_.EnableVoiceProcessing(false);
    }
    application_.audio_service_.SetAsrProvider(AsrProvider::kGemini, nullptr);
    active_provider_ = AsrProvider::kXiaozhi;
    ++turn_id_;
    application_.play_popup_on_listening_ = false;
    client_.Cancel();
}

void GeminiAsrTurnController::HandleApplicationStateChanged(DeviceState new_state) {
    if (new_state != kDeviceStateSpeaking) {
        prewarm_retry_pending_ = false;
    }
    if (new_state != kDeviceStateListening && IsGeminiActive()) {
        Stop();
    }
    if (new_state != kDeviceStateListening) {
        restart_pending_ = false;
        listening_deadline_us_ = 0;
        ready_.store(false);
        preparing_.store(false);
    }
}

void GeminiAsrTurnController::SetPreparingForListening() {
    ready_.store(false);
    preparing_.store(true);
}
