#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <model_path.h>
#include "esp_ae_rate_cvt.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include "audio_codec.h"
#include "audio_engine.h"
#include "acoustic_environment_status.h"
#include "asr_settings.h"
#include "fixed_queue.h"
#include "ogg_demuxer.h"
#include "protocol.h"
#include "voice_input_config.h"

class GeminiTranscribeClient;

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 *                         \-> {Gemini PCM Queue}
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use dedicated tasks for input, output, and Opus encoding/decoding.
 *
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM
 * packets.
 *
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (1200 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING (1 << 2)
#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST (1 << 4)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                  \
    ((duration_ms) == 5     ? ESP_OPUS_ENC_FRAME_DURATION_5_MS   \
     : (duration_ms) == 10  ? ESP_OPUS_ENC_FRAME_DURATION_10_MS  \
     : (duration_ms) == 20  ? ESP_OPUS_ENC_FRAME_DURATION_20_MS  \
     : (duration_ms) == 40  ? ESP_OPUS_ENC_FRAME_DURATION_40_MS  \
     : (duration_ms) == 60  ? ESP_OPUS_ENC_FRAME_DURATION_60_MS  \
     : (duration_ms) == 80  ? ESP_OPUS_ENC_FRAME_DURATION_80_MS  \
     : (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS \
     : (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS \
                            : -1)

#define AS_OPUS_ENC_CONFIG()                                                                   \
    {                                                                                          \
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,                                              \
        .channel = ESP_AUDIO_MONO,                                                             \
        .bits_per_sample = ESP_AUDIO_BIT16,                                                    \
        .bitrate = ESP_OPUS_BITRATE_AUTO,                                                      \
        .frame_duration =                                                                      \
            (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS), \
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,                                    \
        .complexity = 0,                                                                       \
        .enable_fec = false,                                                                   \
        .enable_dtx = true,                                                                    \
        .enable_vbr = true,                                                                    \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // Fired when the decode/playback queues and their in-flight work are drained.
    std::function<void(void)> on_playback_drained;
    std::function<void(uint32_t playback_id, uint32_t media_position_ms)> on_playback_progress;
};

enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
    uint32_t playback_id = 0;
    uint32_t media_position_ms = 0;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
    uint32_t encode_drop_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    uint8_t GetInputLevel() const;
    bool IsInputClipping() const;
    AcousticEnvironmentStatus GetAcousticEnvironmentStatus() const;
    void ConfigureVoiceInput(const VoiceInputConfig& config);
    VoiceInputStatus GetVoiceInputStatus() const;
    void SetAcousticMotionActive(bool active) {
        acoustic_motion_active_.store(active, std::memory_order_relaxed);
    }
    bool IsIdle();
    bool IsPlaybackIdle();
    // Monotonic modulo-2^32 PCM output clock; readers use unsigned deltas.
    // No UI callbacks or locks are needed on the audio output task.
    uint32_t GetOutputClockUs() const { return output_clock_us_.load(std::memory_order_relaxed); }
    bool IsWakeWordRunning() const {
        return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING;
    }
    bool IsAudioProcessorRunning() const {
        return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING;
    }
    bool IsAfeWakeWord();

    void EnableWakeWordDetection(bool enable);
    void ReleaseWakeWordResources();
    void EnableVoiceProcessing(bool enable);
    void SetMicrophoneMuted(bool muted);
    bool IsMicrophoneMuted() const { return microphone_muted_.load(); }
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);

    // Configure between listening turns while voice processing is disabled.
    // Gemini frames never fall back to Xiaozhi when the client is null or not ready.
    // Any non-null Gemini client must outlive this route configuration.
    void SetAsrProvider(AsrProvider provider, GeminiTranscribeClient* gemini_client);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::atomic<uint32_t> output_clock_us_{0};
    std::unique_ptr<AudioEngine> audio_engine_;
    std::atomic<AsrProvider> asr_provider_{AsrProvider::kXiaozhi};
    std::atomic<GeminiTranscribeClient*> gemini_client_{nullptr};
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;

    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    int64_t last_encode_drop_log_time_ = 0;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    // Testing records up to AUDIO_TESTING_MAX_DURATION_MS, then swaps into the
    // decode queue. Both queues must share this capacity so swap() is valid.
    static constexpr size_t kAudioTestingPacketCapacity =
        AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS;
    FixedQueue<std::unique_ptr<AudioStreamPacket>, kAudioTestingPacketCapacity> audio_decode_queue_;
    FixedQueue<std::unique_ptr<AudioStreamPacket>, MAX_SEND_PACKETS_IN_QUEUE> audio_send_queue_;
    FixedQueue<std::unique_ptr<AudioStreamPacket>, kAudioTestingPacketCapacity>
        audio_testing_queue_;
    FixedQueue<AudioTask, MAX_ENCODE_TASKS_IN_QUEUE> audio_encode_queue_;
    FixedQueue<AudioTask, MAX_PLAYBACK_TASKS_IN_QUEUE> audio_playback_queue_;
    bool decode_in_flight_ = false;
    bool output_in_flight_ = false;
    bool playback_drained_notified_ = true;
    uint32_t playback_generation_ = 0;
    // For server AEC
    FixedQueue<uint32_t, MAX_TIMESTAMPS_IN_QUEUE> timestamp_queue_;

    bool audio_engine_initialized_ = false;
    bool voice_detected_ = false;
#if CONFIG_USE_DEVICE_AEC
    bool device_aec_enabled_ = true;
#else
    bool device_aec_enabled_ = false;
#endif
    std::atomic<bool> service_stopped_{true};
    std::atomic<bool> microphone_muted_{false};
    std::atomic<bool> audio_input_need_warmup_{false};
    std::atomic<uint8_t> input_level_{0};
    std::atomic<int64_t> last_input_level_us_{0};
    std::atomic<int64_t> last_input_clip_us_{0};

    // PCM-derived room telemetry. The input task is the sole accumulator writer;
    // a sequence counter publishes coherent lock-free snapshots to status readers.
    static constexpr int64_t kAcousticFreshnessUs = 750 * 1000LL;
    std::atomic<uint32_t> acoustic_snapshot_sequence_{0};
    std::atomic<bool> acoustic_snapshot_valid_{false};
    std::atomic<bool> acoustic_floor_valid_{false};
    std::atomic<float> acoustic_rms_dbfs_{-90.0f};
    std::atomic<float> acoustic_peak_dbfs_{-90.0f};
    std::atomic<float> acoustic_noise_floor_dbfs_{-90.0f};
    std::atomic<float> acoustic_signal_over_floor_db_{0.0f};
    std::atomic<bool> acoustic_snapshot_self_noise_{false};
    std::atomic<int64_t> acoustic_last_update_us_{0};
    std::atomic<int64_t> acoustic_valid_after_us_{0};
    std::atomic<bool> acoustic_motion_active_{false};
    std::atomic<bool> acoustic_playback_active_{false};
    std::atomic<int64_t> acoustic_last_playback_us_{0};
    std::atomic<bool> acoustic_reset_accumulator_{false};
    std::atomic<bool> acoustic_reset_floor_{false};
    uint64_t acoustic_sum_squares_ = 0;
    int64_t acoustic_sum_ = 0;
    size_t acoustic_sample_count_ = 0;
    uint32_t acoustic_peak_ = 0;
    bool acoustic_window_self_noise_ = false;
    bool acoustic_noise_floor_initialized_ = false;
    float acoustic_noise_floor_estimate_dbfs_ = -90.0f;
    float acoustic_floor_initial_min_dbfs_ = 0.0f;
    uint8_t acoustic_floor_initial_samples_ = 0;
    uint8_t acoustic_elevated_samples_ = 0;
    float acoustic_elevated_min_dbfs_ = 0.0f;

    struct PcmLevelMeter {
        uint64_t sum_squares = 0;
        int64_t sum = 0;
        size_t sample_count = 0;
        uint32_t peak = 0;
        std::atomic<uint32_t> sequence{0};
        std::atomic<float> rms_dbfs{-90.0f};
        std::atomic<float> peak_dbfs{-90.0f};
        std::atomic<int64_t> last_update_us{0};
    };
    std::atomic<VoiceInputProfile> voice_profile_{VoiceInputProfile::kLegacy};
    std::atomic<int> capture_trim_db_{0};
    std::atomic<int> voice_gain_db_{0};
    std::atomic<float> voice_gain_scale_{1.0f};
    std::atomic<bool> voice_ns_requested_{false};
    std::atomic<bool> voice_agc_requested_{false};
    std::atomic<int64_t> voice_diagnostics_valid_after_us_{0};
    std::atomic<bool> voice_level_reset_pending_{false};
    std::atomic<bool> afe_output_level_reset_pending_{false};
    PcmLevelMeter voice_level_meter_;
    PcmLevelMeter afe_output_level_meter_;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    bool InitializeAudioEngine();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
    bool IsPlaybackDrainedLocked() const;
    bool MarkPlaybackDrainedLocked();
    void UpdateAcousticTelemetry(const std::vector<int16_t>& data, int sample_rate,
                                 uint32_t frame_peak, int64_t now_us, bool input_available);
    void ResetAcousticAccumulator();
    void ResetAcousticFloor();
    void ApplyVoiceGainAndMeasure(std::vector<int16_t>& data, int sample_rate);
    void ResetPcmLevelAccumulator(PcmLevelMeter& meter);
    void UpdatePcmLevel(PcmLevelMeter& meter, const int16_t* data, size_t sample_count,
                        int sample_rate);
    void PublishPcmLevelIfReady(PcmLevelMeter& meter, size_t added_samples, int sample_rate);
    PcmLevelStatus GetPcmLevelStatus(const PcmLevelMeter& meter) const;
};

#endif
