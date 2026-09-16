#pragma once

#include "display/lcd_display.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class MochanDisplay : public SpiLcdDisplay {
public:
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
    void SetWifiConnected(bool connected);
    void SetBatteryStatus(int percent, float voltage_v, bool charging);
    bool SetPanelMirror(bool mirror_x, bool mirror_y);
    std::string GetCurrentEmotion() const;
    static bool IsSupportedEmotion(const std::string& emotion);

    void ShowBootSplash();
    void HideBootSplash();

private:
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
        std::string rendered_emotion;
        uint16_t previous_scale_x = 0;
        uint16_t previous_scale_y = 0;
        uint8_t previous_opacity = 0;
        int previous_x = 0;
        int previous_y = 0;
        int previous_pivot_y = -1;
    };

    EyeRaster left_raster_;
    EyeRaster right_raster_;
    MouthRaster mouth_raster_;
    bool InitializeEyeRasters();
    bool InitializeMouthRaster();
    bool RenderEyeRaster(EyeRaster& raster, const EyeGeometry& geometry, uint8_t blink_amount);
    bool RenderMouthTarget(const std::string& emotion);
    void UpdateMouth(uint8_t blink_amount, const std::string& emotion);
    static bool HasMouthGeometry(const std::string& emotion);
    static bool GetMouthIdleEyeOffset(const std::string& emotion, int& offset_y);

    void SetFaceState(FaceState state);
    void AdvanceEyeAnimation();
    void AdvanceFaceLayout(int64_t now_us);
    void AdvanceIdleScheduler(std::string& emotion);
    void AdvanceIdleMouthAnimation(bool idle_eligible);
    void CancelIdleScheduler(bool restart_session);
    void ApplyIdleEmotion(const char* emotion);
    void UpdateEyes(uint8_t blink_amount, bool idle_eligible);
    void ApplyRoundedEye(lv_obj_t* eye, lv_obj_t* shadow, const EyeGeometry& geometry,
                         uint8_t blink_amount);
    static bool AllowsNaturalBlink(FaceState state);
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
    lv_obj_t* response_box_ = nullptr;
    lv_obj_t* subtitle_ = nullptr;
    lv_obj_t* notification_ = nullptr;
    lv_obj_t* camera_image_ = nullptr;
    lv_obj_t* splash_ = nullptr;
    lv_obj_t* wifi_icon_ = nullptr;
    lv_obj_t* battery_icon_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* battery_status_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;
    lv_timer_t* notification_timer_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> camera_image_cached_;
    std::string typing_text_;
    size_t typing_position_ = 0;
    int32_t response_scroll_target_ = 0;
    int64_t typing_last_update_us_ = 0;
    int64_t typing_glyph_credit_ = 0;
    uint8_t typing_cursor_phase_ = 0;
    bool typing_cursor_visible_ = false;
    bool typing_active_ = false;
    bool typing_finishing_ = false;
    FaceState face_state_ = FaceState::kIdle;
    FaceState activity_state_ = FaceState::kIdle;
    bool status_dot_busy_ = false;
    bool emotion_active_ = false;
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
    int64_t max_callback_duration_us_ = 0;
    int64_t idle_session_started_ms_ = 0;
    int64_t next_idle_emotion_ms_ = 0;
    int64_t last_yawn_ms_ = 0;
    int64_t yawn_started_ms_ = 0;
    int64_t next_mouth_motion_ms_ = 0;
    int64_t mouth_motion_started_ms_ = 0;
    uint16_t yawn_amount_ = 0;
    int16_t mouth_motion_amount_ = 0;
    uint16_t idle_motion_phase_ = 0;
    int8_t idle_gaze_x_ = 0;
    int8_t idle_gaze_y_ = 0;
    uint8_t idle_repeat_count_ = 0;
    bool idle_override_active_ = false;
    bool yawn_active_ = false;
    bool mouth_motion_active_ = false;
    bool response_box_requested_ = false;
    bool preview_show_pending_ = false;
    std::string last_idle_emotion_;
    std::string exiting_mouth_emotion_;
    bool wifi_connected_ = false;
    mutable std::mutex emotion_mutex_;
    std::string current_emotion_ = "neutral";
};
