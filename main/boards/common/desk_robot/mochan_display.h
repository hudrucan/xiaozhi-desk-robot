#pragma once

#include "display/lcd_display.h"

#include <cstddef>
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
    };
    EyeRaster left_raster_;
    EyeRaster right_raster_;
    bool InitializeEyeRasters();
    void RenderEyeRaster(EyeRaster& raster, const EyeGeometry& geometry, uint8_t blink_amount);

    void SetFaceState(FaceState state);
    void AdvanceEyeAnimation();
    void UpdateEyes(uint8_t blink_amount);
    void ApplyRoundedEye(lv_obj_t* eye, lv_obj_t* shadow, const EyeGeometry& geometry,
                         uint8_t blink_amount);
    static bool AllowsNaturalBlink(FaceState state);
    void UpdateStatusDot();
    void ShowResponseBox();
    void HidePreview();
    void HideNotification();
    void StartTyping(const char* content);
    void UpdateTyping();
    void FinishTyping();
    void ResetTyping();
    void RenderTypingText();

    lv_obj_t* face_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_eyelid_ = nullptr;
    lv_obj_t* right_eyelid_ = nullptr;
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
    lv_timer_t* typing_timer_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> camera_image_cached_;
    std::string typing_text_;
    size_t typing_position_ = 0;
    int32_t response_scroll_target_ = 0;
    uint8_t typing_cursor_phase_ = 0;
    bool typing_cursor_visible_ = false;
    bool typing_active_ = false;
    bool typing_finishing_ = false;
    FaceState face_state_ = FaceState::kIdle;
    FaceState activity_state_ = FaceState::kIdle;
    bool emotion_active_ = false;
    uint16_t animation_phase_ = 0;
    uint16_t blink_countdown_ = 90;
    uint8_t blink_step_ = 0;
    bool eye_geometry_initialized_ = false;
    EyeGeometry left_eye_geometry_{74, 54, -48, -55, 0};
    EyeGeometry right_eye_geometry_{74, 54, 48, -55, 0};
    bool wifi_connected_ = false;
    mutable std::mutex emotion_mutex_;
    std::string current_emotion_ = "neutral";
};
