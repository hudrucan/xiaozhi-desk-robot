#pragma once

#include "behavior/ambient_behavior_types.h"
#include "display/lcd_display.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class MochanDisplay : public SpiLcdDisplay {
public:
    enum class AmbientActivity : uint8_t {
        kIdle,
        kListening,
        kThinking,
        kSpeaking,
        kSuppressed,
    };

    MochanDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
    ~MochanDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void SetTheme(Theme* theme) override;
    void SetMicrophoneMuted(bool muted) override;
    void SetWifiConnected(bool connected);
    void SetBatteryStatus(int percent, float voltage_v, bool charging);
    void SetFaceOverrideActive(bool active);
    void RestoreActivityFace();
    bool SetPanelMirror(bool mirror_x, bool mirror_y);
    std::string GetCurrentEmotion() const;
    AmbientActivity GetAmbientActivity() const;
    AmbientGazePersonality GetAmbientGazePersonality() const;
    void ResetAmbientPrimitives(uint32_t generation);
    void SetAmbientGazeTarget(uint32_t generation, int8_t x, int8_t y);
    void ClearAmbientGaze(uint32_t generation);
    void TriggerAmbientMouth(uint32_t generation);
    void TriggerAmbientYawn(uint32_t generation);
    void SetAmbientBaseFace(uint32_t generation, const std::string& emotion);
    void ClearAmbientBaseFace(uint32_t generation);
    static bool IsSupportedEmotion(const std::string& emotion);

    void ShowBootSplash();
    void HideBootSplash();

private:
    enum class AmbientMouthProfile : uint8_t {
        kNeutral,
        kPlayful,
        kCurious,
        kRelaxed,
        kSleepy,
    };

    enum class AmbientMouthVariant : uint8_t {
        kMicroPulse,
        kSoftOpenClose,
        kDoubleTwitch,
        kCount,
    };

    enum class FaceState {
        kIdle,
        kListening,
        kSpeaking,
        kThinking,
        kHappy,
        kLaughing,
        kFunny,
        kAngry,
        kSad,
        kCrying,
        kLoving,
        kEmbarrassed,
        kSurprised,
        kShocked,
        kWinking,
        kCool,
        kRelaxed,
        kDelicious,
        kKissy,
        kConfident,
        kSleepy,
        kSilly,
        kConfused,
        kSuspicious,
        kShake,
        kLookLeft,
        kLookRight,
        kLookUp,
        kLookDown,
        kLookUpLeft,
        kLookUpRight,
        kLookDownLeft,
        kLookDownRight,
    };

    struct EyeGeometry {
        int width;
        int height;
        int x;
        int y;
        int rotation;
        int top_curve = 0;
        int bottom_curve = 0;
        int slope = 0;
        int water = 0;
    };

    struct EyeRaster {
        static constexpr int kWidth = 96;
        static constexpr int kHeight = 88;
        uint32_t* pixels = nullptr;
        lv_image_dsc_t descriptor{};
        EyeGeometry previous{};
        uint8_t previous_blink = 0;
        bool rendered = false;
        bool positioned = false;
        int displayed_x = 0;
        int displayed_y = 0;
        int displayed_rotation = 0;
    };

    struct MouthRaster {
        static constexpr int kWidth = 120;
        static constexpr int kHeight = 80;
        uint32_t* pixels = nullptr;
        lv_image_dsc_t descriptor{};
        uint16_t previous_scale_x = 0;
        uint16_t previous_scale_y = 0;
        uint8_t previous_opacity = 0;
        int previous_x = 0;
        int previous_y = 0;
        int previous_pivot_y = -1;
    };

    struct MouthMorphState {
        float width = 72.0f;
        float height = 24.0f;
        float top_curve = 2.0f;
        float bottom_curve = 2.0f;
        float slope = 0.0f;
        float gap = 74.0f;
        bool initialized = false;
        float rendered_width = 0.0f;
        float rendered_height = 0.0f;
        float rendered_top_curve = 0.0f;
        float rendered_bottom_curve = 0.0f;
        float rendered_slope = 0.0f;
    };

    struct SleepyZzRaster {
        static constexpr int kWidth = 40;
        static constexpr int kHeight = 40;
        uint32_t* pixels = nullptr;
        lv_image_dsc_t descriptor{};
        float alpha = 0.0f;
        int hold_ticks = 0;
        uint8_t previous_opacity = 0;
        bool positioned = false;
        int displayed_x = 0;
        int displayed_y = 0;
    };

    EyeRaster left_raster_;
    EyeRaster right_raster_;
    MouthRaster mouth_raster_;
    MouthMorphState mouth_morph_state_;
    SleepyZzRaster sleepy_zz_raster_;
    int mouth_shape_opacity_ = 0;
    bool InitializeEyeRasters();
    bool InitializeMouthRaster();
    bool InitializeSleepyZzRaster();
    bool RenderEyeRaster(EyeRaster& raster, const EyeGeometry& geometry, uint8_t blink_amount);
    void AdvanceSleepyZzAnimation(bool visual_eligible);
    void UpdateMouth(uint8_t blink_amount, const std::string& emotion);
    static bool HasMouthGeometry(const std::string& emotion);
    static bool GetMouthIdleEyeOffset(const std::string& emotion, int& offset_y);

    void SetFaceState(FaceState state);
    void ApplyRestingFaceLocked();
    static bool ResolveEmotionFaceState(const std::string& emotion, FaceState& state);
    void AdvanceEyeAnimation();
    void AdvanceFaceLayout(int64_t now_us);
    void ConsumeAmbientPrimitiveRequests(const std::string& emotion, bool idle_eligible);
    void ConsumeAmbientBaseFaceRequest();
    void AdvanceYawnAnimation(const std::string& emotion);
    static AmbientMouthProfile ResolveAmbientMouthProfile(const std::string& emotion);
    void StartAmbientMouthAnimation(const std::string& emotion, int64_t now_ms);
    void AdvanceMouthAnimation(bool idle_eligible);
    void CancelAmbientAnimations();
    void UpdateEyes(uint8_t blink_amount, bool ambient_visual_eligible);
    void ApplyRoundedEye(lv_obj_t* eye, lv_obj_t* shadow, const EyeGeometry& geometry,
                         uint8_t blink_amount);
    static bool AllowsNaturalBlink(FaceState state);
    static bool AllowsAmbientGaze(FaceState state);
    static AmbientGazePersonality ResolveAmbientGazePersonality(FaceState state);
    bool CanShowFullFace(const std::string& emotion) const;
    bool IsIdleEligible(const std::string& emotion) const;
    void SetFaceLayoutTarget(uint16_t target, int64_t now_us);
    void UpdateFaceLayoutTarget(const std::string& emotion, int64_t now_us);
    void FreezeMouthForExit();
    void AdvanceResponseBoxTransition(int64_t now_us);
    void RecordAnimationTiming(int64_t callback_started_us, int64_t frame_interval_us);
    void UpdateStatusDot();
    void ShowResponseBox();
    void HideResponseBox();
    void HidePreview();
    void HideNotification();
    void StartTyping(const char* content);
    void UpdateTyping(int64_t now_us);
    void FinishTyping();
    void ResetTyping();
    void RenderTypingText();

    lv_obj_t* face_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_eyelid_ = nullptr;
    lv_obj_t* right_eyelid_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* sleepy_zz_ = nullptr;
    lv_obj_t* response_box_ = nullptr;
    lv_obj_t* subtitle_ = nullptr;
    lv_obj_t* notification_ = nullptr;
    lv_obj_t* camera_image_ = nullptr;
    lv_obj_t* splash_ = nullptr;
    lv_obj_t* wifi_icon_ = nullptr;
    lv_obj_t* battery_icon_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* mic_mute_icon_ = nullptr;
    lv_obj_t* battery_status_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;
    lv_timer_t* notification_timer_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> camera_image_cached_;
    std::string typing_text_;
    std::string typing_rendered_text_;
    size_t typing_window_start_ = 0;
    size_t typing_position_ = 0;
    int32_t response_scroll_target_ = 0;
    int64_t typing_last_update_us_ = 0;
    uint32_t typing_output_clock_us_ = 0;
    int64_t typing_glyph_credit_ = 0;
    uint8_t typing_cursor_phase_ = 0;
    bool typing_cursor_visible_ = false;
    bool typing_active_ = false;
    bool typing_finishing_ = false;
    FaceState face_state_ = FaceState::kIdle;
    FaceState activity_state_ = FaceState::kIdle;
    std::atomic<AmbientActivity> ambient_activity_{AmbientActivity::kIdle};
    std::atomic<AmbientGazePersonality> ambient_gaze_personality_{
        AmbientGazePersonality::kNeutral};
    bool status_dot_busy_ = false;
    bool emotion_active_ = false;
    std::atomic_bool face_override_active_{false};
    uint16_t animation_phase_ = 0;
    uint16_t blink_countdown_ = 90;
    uint8_t blink_step_ = 0;
    bool eye_geometry_initialized_ = false;
    EyeGeometry left_eye_geometry_{74, 54, -48, -55, 0};
    EyeGeometry right_eye_geometry_{74, 54, 48, -55, 0};
    uint16_t face_layout_progress_ = 0;
    uint16_t face_layout_from_ = 0;
    uint16_t face_layout_target_ = 0;
    int face_layout_full_offset_y_ = 0;
    int face_layout_full_offset_from_y_ = 0;
    int face_layout_full_offset_target_y_ = 0;
    int64_t face_layout_transition_started_us_ = 0;
    int64_t face_layout_emotion_transition_started_us_ = 0;
    uint16_t response_box_progress_ = 0;
    uint16_t response_box_from_ = 0;
    uint16_t response_box_target_ = 0;
    int64_t response_box_transition_started_us_ = 0;
    uint8_t response_box_opacity_ = 0;
    int face_layout_offset_y_ = 0;
    int64_t last_animation_callback_us_ = 0;
    int64_t last_performance_log_us_ = 0;
    int64_t max_frame_interval_us_ = 0;
    int64_t max_face_work_us_ = 0;
    int64_t max_text_work_us_ = 0;
    int64_t max_callback_duration_us_ = 0;
    int64_t yawn_started_ms_ = 0;
    int64_t mouth_motion_started_ms_ = 0;
    int mouth_motion_duration_ms_ = 0;
    int16_t mouth_motion_peak_ = 0;
    uint16_t yawn_amount_ = 0;
    int16_t mouth_motion_amount_ = 0;
    int8_t ambient_gaze_x_ = 0;
    int8_t ambient_gaze_y_ = 0;
    int8_t ambient_gaze_target_x_ = 0;
    int8_t ambient_gaze_target_y_ = 0;
    bool yawn_active_ = false;
    bool mouth_motion_active_ = false;
    AmbientMouthVariant mouth_motion_variant_ = AmbientMouthVariant::kMicroPulse;
    AmbientMouthVariant last_mouth_motion_variant_ = AmbientMouthVariant::kCount;
    std::mutex ambient_primitive_mutex_;
    uint32_t ambient_primitive_generation_ = 0;
    uint32_t applied_ambient_primitive_generation_ = 0;
    int8_t requested_ambient_gaze_x_ = 0;
    int8_t requested_ambient_gaze_y_ = 0;
    uint32_t ambient_gaze_request_generation_ = 0;
    uint32_t ambient_mouth_request_generation_ = 0;
    uint32_t ambient_yawn_request_generation_ = 0;
    uint32_t ambient_base_face_generation_ = 0;
    uint32_t ambient_base_face_request_generation_ = 0;
    uint32_t applied_ambient_base_face_generation_ = 0;
    std::string requested_ambient_base_face_;
    std::string ambient_base_face_;
    bool ambient_gaze_requested_ = false;
    bool ambient_mouth_requested_ = false;
    bool ambient_yawn_requested_ = false;
    bool ambient_base_face_requested_ = false;
    bool ambient_base_face_request_pending_ = false;
    bool response_box_requested_ = false;
    bool preview_show_pending_ = false;
    std::string exiting_mouth_emotion_;
    bool wifi_connected_ = false;
    mutable std::mutex emotion_mutex_;
    std::string current_emotion_ = "neutral";
};
