#pragma once

#include "audio/voice_input_config.h"

class RobotSettings {
public:
    int GetSpeakerVolume() const;
    bool GetMicrophoneMuted() const;
    void SetMicrophoneMuted(bool muted) const;
    VoiceInputConfig GetVoiceInputConfig() const;
    void SetVoiceInputConfig(const VoiceInputConfig& config) const;

    bool GetCameraFlipped() const;
    void SetCameraFlipped(bool flipped) const;
    bool GetDisplayFlipped() const;
    void SetDisplayFlipped(bool flipped) const;
    int GetCliffEdgeMm() const;
    void SetCliffEdgeMm(int edge_mm) const;
    bool GetMotionEmotionsEnabled() const;
    void SetMotionEmotionsEnabled(bool enabled) const;
    int GetStatusLightBrightness() const;
    void SetStatusLightBrightness(int brightness) const;
    int GetMotorSpeed() const;
    void SetMotorSpeed(int speed) const;
    int GetDriveDurationMs() const;
    void SetDriveDurationMs(int duration_ms) const;
    bool GetEmotionMovementEnabled() const;
    void SetEmotionMovementEnabled(bool enabled) const;
    int GetManualScreenBrightness() const;
    bool GetAutoBrightnessEnabled() const;
    void SetAutoBrightnessEnabled(bool enabled) const;
    int GetAutoBrightnessMinimum() const;
    void SetAutoBrightnessMinimum(int brightness) const;
    int GetAutoBrightnessMaximum() const;
    void SetAutoBrightnessMaximum(int brightness) const;
};
