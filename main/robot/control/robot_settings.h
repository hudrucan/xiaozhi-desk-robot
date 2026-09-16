#pragma once

class RobotSettings {
public:
    int GetSpeakerVolume() const;
    int GetMicrophoneGain() const;
    void SetMicrophoneGain(int gain) const;

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
};
