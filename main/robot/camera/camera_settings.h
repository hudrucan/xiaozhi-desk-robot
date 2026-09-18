#pragma once

#include <cstdint>
#include <mutex>

enum class CameraImageProfile : uint8_t {
    kNormal,
    kLowLight,
    kCustom,
    kAuto,
};

enum class CameraResolution : uint8_t {
    kAuto,
    kQvga,
    kHvga,
    kVga,
    kSvga,
    kXga,
    kSxga,
    kUxga,
};

enum class CameraGainCeiling : uint8_t {
    k2x,
    k4x,
    k8x,
    k16x,
    k32x,
    k64x,
    k128x,
};

enum class MochanAspectMode : uint8_t {
    kAuto,
    kSquare,
    kFourThree,
    kThreeTwo,
    kSixteenNine,
};

enum class MochanRenderMode : uint8_t {
    kFillCrop,
    kFit,
};

enum class McpFreshFramePolicy : uint8_t {
    kLatest,
    kFresh,
};

struct CameraSensorSettings {
    CameraImageProfile profile = CameraImageProfile::kNormal;
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;
    bool auto_exposure = true;
    bool aec2 = false;
    int ae_level = 0;
    int manual_exposure = 300;
    bool auto_gain = true;
    int manual_gain = 0;
    CameraGainCeiling gain_ceiling = CameraGainCeiling::k8x;
    bool auto_white_balance = true;
    bool awb_gain = true;
    int white_balance_mode = 0;
    bool black_pixel_correction = false;
    bool white_pixel_correction = true;
    bool gamma = true;
    bool lens_correction = true;
    bool mirror = false;
    bool flip = false;
};

struct WebCameraSettings {
    CameraResolution resolution = CameraResolution::kVga;
    int jpeg_quality = 12;
    int fps = 6;
};

struct MochanCameraSettings {
    CameraResolution source_resolution = CameraResolution::kAuto;
    MochanAspectMode aspect = MochanAspectMode::kAuto;
    MochanRenderMode render = MochanRenderMode::kFillCrop;
};

struct McpCameraSettings {
    CameraResolution resolution = CameraResolution::kVga;
    int jpeg_quality = 12;
    McpFreshFramePolicy freshness = McpFreshFramePolicy::kFresh;
};

struct CameraSettingsConfig {
    CameraSensorSettings sensor;
    WebCameraSettings web;
    MochanCameraSettings mochan;
    McpCameraSettings mcp;
};

class CameraSettingsStore {
public:
    void Load(bool legacy_flipped);
    CameraSettingsConfig Get() const;
    void Save(const CameraSettingsConfig& config);
    void ResetToDefaults(bool flipped = false);
    void SetOrientation(bool mirror, bool flip);

    static CameraSettingsConfig Defaults(bool flipped = false);
    static CameraSettingsConfig Normalize(CameraSettingsConfig config);

private:
    static void Persist(const CameraSettingsConfig& config);

    mutable std::mutex mutex_;
    CameraSettingsConfig config_;
};
