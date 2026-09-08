#pragma once

#include "display/lcd_display.h"

#include <memory>
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

    void ShowBootSplash();
    void HideBootSplash();

private:
    enum class FaceState {
        kIdle,
        kListening,
        kSpeaking,
        kThinking,
        kHappy,
        kAngry,
        kSad,
        kSuspicious,
        kShake,
        kLookUpLeft,
        kLookUpRight,
        kLookDownLeft,
        kLookDownRight,
    };

    void SetFaceState(FaceState state);
    void UpdateEyes(bool blink);
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
    lv_obj_t* status_dot_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;
    lv_timer_t* notification_timer_ = nullptr;
    lv_timer_t* typing_timer_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> camera_image_cached_;
    std::string typing_text_;
    size_t typing_position_ = 0;
    uint8_t typing_cursor_phase_ = 0;
    bool typing_cursor_visible_ = false;
    bool typing_active_ = false;
    bool typing_finishing_ = false;
    FaceState face_state_ = FaceState::kIdle;
    FaceState activity_state_ = FaceState::kIdle;
    bool emotion_active_ = false;
    bool blink_closed_ = false;
    uint8_t blink_phase_ = 0;
    bool eye_geometry_initialized_ = false;
    int eye_width_ = 74;
    int eye_height_ = 54;
    int eye_gap_ = 22;
    int eye_gaze_x_ = 0;
    int eye_gaze_y_ = -59;
    int left_eye_rotation_ = 0;
    int right_eye_rotation_ = 0;
    bool wifi_connected_ = false;
};
