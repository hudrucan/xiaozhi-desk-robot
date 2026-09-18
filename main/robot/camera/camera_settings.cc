#include "camera_settings.h"

#include "settings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace {

constexpr char kNamespace[] = "camera";
constexpr int kSchemaVersion = 1;

constexpr char kVersionKey[] = "version";
constexpr char kProfileKey[] = "profile";
constexpr char kBrightnessKey[] = "brightness";
constexpr char kContrastKey[] = "contrast";
constexpr char kSaturationKey[] = "saturation";
constexpr char kAecKey[] = "aec";
constexpr char kAec2Key[] = "aec2";
constexpr char kAeLevelKey[] = "ae_level";
constexpr char kExposureKey[] = "exposure";
constexpr char kAgcKey[] = "agc";
constexpr char kGainKey[] = "gain";
constexpr char kGainCeilingKey[] = "gain_ceiling";
constexpr char kAwbKey[] = "awb";
constexpr char kAwbGainKey[] = "awb_gain";
constexpr char kWhiteBalanceModeKey[] = "wb_mode";
constexpr char kBpcKey[] = "bpc";
constexpr char kWpcKey[] = "wpc";
constexpr char kGammaKey[] = "gamma";
constexpr char kLensCorrectionKey[] = "lenc";
constexpr char kMirrorKey[] = "mirror";
constexpr char kFlipKey[] = "flip";
constexpr char kWebResolutionKey[] = "web_res";
constexpr char kWebQualityKey[] = "web_quality";
constexpr char kWebFpsKey[] = "web_fps";
constexpr char kMochanResolutionKey[] = "mochan_res";
constexpr char kMochanAspectKey[] = "mochan_aspect";
constexpr char kMochanRenderKey[] = "mochan_render";
constexpr char kMcpResolutionKey[] = "mcp_res";
constexpr char kMcpQualityKey[] = "mcp_quality";
constexpr char kMcpFreshnessKey[] = "mcp_fresh";

constexpr size_t kNvsNameMaxLength = 15;
constexpr std::array<std::string_view, 30> kNvsNames = {
    kNamespace,          kVersionKey,          kProfileKey,        kBrightnessKey,
    kContrastKey,        kSaturationKey,       kAecKey,            kAec2Key,
    kAeLevelKey,         kExposureKey,         kAgcKey,            kGainKey,
    kGainCeilingKey,     kAwbKey,              kAwbGainKey,        kWhiteBalanceModeKey,
    kBpcKey,             kWpcKey,              kGammaKey,          kLensCorrectionKey,
    kMirrorKey,          kFlipKey,             kWebResolutionKey,  kWebQualityKey,
    kWebFpsKey,          kMochanResolutionKey, kMochanAspectKey,   kMochanRenderKey,
    kMcpResolutionKey,   kMcpQualityKey,
};
static_assert([] {
    for (const std::string_view name : kNvsNames) {
        if (name.size() > kNvsNameMaxLength) {
            return false;
        }
    }
    return std::string_view(kMcpFreshnessKey).size() <= kNvsNameMaxLength;
}());

template <typename Enum>
Enum ReadEnum(Settings& settings, const char* key, Enum fallback, int maximum) {
    const int value = settings.GetInt(key, static_cast<int>(fallback));
    if (value < 0 || value > maximum) {
        return fallback;
    }
    return static_cast<Enum>(value);
}

template <typename Enum>
Enum NormalizeEnum(Enum value, Enum maximum, Enum fallback) {
    const int integer = static_cast<int>(value);
    return integer >= 0 && integer <= static_cast<int>(maximum) ? value : fallback;
}

bool IsOneOf(CameraResolution resolution,
             std::initializer_list<CameraResolution> allowed) {
    return std::find(allowed.begin(), allowed.end(), resolution) != allowed.end();
}

CameraResolution NormalizeWebResolution(CameraResolution resolution) {
    return IsOneOf(resolution, {CameraResolution::kQvga, CameraResolution::kHvga,
                                CameraResolution::kVga, CameraResolution::kSvga})
               ? resolution
               : CameraResolution::kVga;
}

CameraResolution NormalizeMochanResolution(CameraResolution resolution) {
    return IsOneOf(resolution, {CameraResolution::kAuto, CameraResolution::kQvga,
                                CameraResolution::kHvga, CameraResolution::kVga})
               ? resolution
               : CameraResolution::kAuto;
}

CameraResolution NormalizeMcpResolution(CameraResolution resolution) {
    return IsOneOf(resolution, {CameraResolution::kVga, CameraResolution::kSvga,
                                CameraResolution::kXga, CameraResolution::kSxga,
                                CameraResolution::kUxga})
               ? resolution
               : CameraResolution::kVga;
}

}  // namespace

CameraSettingsConfig CameraSettingsStore::Defaults(bool flipped) {
    CameraSettingsConfig config;
    config.sensor.mirror = flipped;
    config.sensor.flip = flipped;
    return config;
}

CameraSettingsConfig CameraSettingsStore::Normalize(CameraSettingsConfig config) {
    config.sensor.profile =
        NormalizeEnum(config.sensor.profile, CameraImageProfile::kAuto,
                      CameraImageProfile::kNormal);
    config.sensor.brightness = std::clamp(config.sensor.brightness, -2, 2);
    config.sensor.contrast = std::clamp(config.sensor.contrast, -2, 2);
    config.sensor.saturation = std::clamp(config.sensor.saturation, -2, 2);
    config.sensor.ae_level = std::clamp(config.sensor.ae_level, -2, 2);
    config.sensor.manual_exposure = std::clamp(config.sensor.manual_exposure, 0, 1200);
    config.sensor.manual_gain = std::clamp(config.sensor.manual_gain, 0, 30);
    config.sensor.gain_ceiling =
        NormalizeEnum(config.sensor.gain_ceiling, CameraGainCeiling::k128x,
                      CameraGainCeiling::k2x);
    config.sensor.white_balance_mode = std::clamp(config.sensor.white_balance_mode, 0, 4);
    if (config.sensor.profile != CameraImageProfile::kCustom) {
        const bool mirror = config.sensor.mirror;
        const bool flip = config.sensor.flip;
        CameraSensorSettings preset;
        preset.profile = config.sensor.profile;
        preset.mirror = mirror;
        preset.flip = flip;
        if (preset.profile == CameraImageProfile::kLowLight) {
            preset.auto_exposure = true;
            preset.aec2 = true;
            preset.ae_level = 1;
            preset.auto_gain = true;
            preset.gain_ceiling = CameraGainCeiling::k16x;
            preset.auto_white_balance = true;
            preset.awb_gain = true;
            preset.black_pixel_correction = true;
            preset.white_pixel_correction = true;
            preset.gamma = true;
            preset.lens_correction = true;
        }
        config.sensor = preset;
    }
    config.web.resolution = NormalizeWebResolution(config.web.resolution);
    config.web.jpeg_quality = std::clamp(config.web.jpeg_quality, 4, 63);
    config.web.fps = std::clamp(config.web.fps, 1, 30);
    config.mochan.source_resolution = NormalizeMochanResolution(config.mochan.source_resolution);
    config.mochan.aspect =
        NormalizeEnum(config.mochan.aspect, MochanAspectMode::kSixteenNine,
                      MochanAspectMode::kAuto);
    config.mochan.render =
        NormalizeEnum(config.mochan.render, MochanRenderMode::kFit,
                      MochanRenderMode::kFillCrop);
    config.mcp.resolution = NormalizeMcpResolution(config.mcp.resolution);
    config.mcp.jpeg_quality = std::clamp(config.mcp.jpeg_quality, 4, 63);
    config.mcp.freshness =
        NormalizeEnum(config.mcp.freshness, McpFreshFramePolicy::kFresh,
                      McpFreshFramePolicy::kFresh);
    return config;
}

void CameraSettingsStore::Load(bool legacy_flipped) {
    Settings settings(kNamespace);
    // A missing schema uses the legacy 180-degree flip as its orientation
    // default. Migration is written lazily by the first explicit settings
    // update, avoiding a large NVS write during boot.
    CameraSettingsConfig config = Defaults(legacy_flipped);
    const bool has_current_schema = settings.GetInt(kVersionKey, 0) == kSchemaVersion;
    if (has_current_schema) {
        config.sensor.profile =
            ReadEnum(settings, kProfileKey, config.sensor.profile,
                     static_cast<int>(CameraImageProfile::kAuto));
        config.sensor.brightness = settings.GetInt(kBrightnessKey, config.sensor.brightness);
        config.sensor.contrast = settings.GetInt(kContrastKey, config.sensor.contrast);
        config.sensor.saturation = settings.GetInt(kSaturationKey, config.sensor.saturation);
        config.sensor.auto_exposure = settings.GetBool(kAecKey, config.sensor.auto_exposure);
        config.sensor.aec2 = settings.GetBool(kAec2Key, config.sensor.aec2);
        config.sensor.ae_level = settings.GetInt(kAeLevelKey, config.sensor.ae_level);
        config.sensor.manual_exposure =
            settings.GetInt(kExposureKey, config.sensor.manual_exposure);
        config.sensor.auto_gain = settings.GetBool(kAgcKey, config.sensor.auto_gain);
        config.sensor.manual_gain = settings.GetInt(kGainKey, config.sensor.manual_gain);
        config.sensor.gain_ceiling =
            ReadEnum(settings, kGainCeilingKey, config.sensor.gain_ceiling,
                     static_cast<int>(CameraGainCeiling::k128x));
        config.sensor.auto_white_balance =
            settings.GetBool(kAwbKey, config.sensor.auto_white_balance);
        config.sensor.awb_gain = settings.GetBool(kAwbGainKey, config.sensor.awb_gain);
        config.sensor.white_balance_mode =
            settings.GetInt(kWhiteBalanceModeKey, config.sensor.white_balance_mode);
        config.sensor.black_pixel_correction =
            settings.GetBool(kBpcKey, config.sensor.black_pixel_correction);
        config.sensor.white_pixel_correction =
            settings.GetBool(kWpcKey, config.sensor.white_pixel_correction);
        config.sensor.gamma = settings.GetBool(kGammaKey, config.sensor.gamma);
        config.sensor.lens_correction =
            settings.GetBool(kLensCorrectionKey, config.sensor.lens_correction);
        config.sensor.mirror = settings.GetBool(kMirrorKey, config.sensor.mirror);
        config.sensor.flip = settings.GetBool(kFlipKey, config.sensor.flip);
        config.web.resolution =
            ReadEnum(settings, kWebResolutionKey, config.web.resolution,
                     static_cast<int>(CameraResolution::kUxga));
        config.web.jpeg_quality = settings.GetInt(kWebQualityKey, config.web.jpeg_quality);
        config.web.fps = settings.GetInt(kWebFpsKey, config.web.fps);
        config.mochan.source_resolution =
            ReadEnum(settings, kMochanResolutionKey, config.mochan.source_resolution,
                     static_cast<int>(CameraResolution::kUxga));
        config.mochan.aspect =
            ReadEnum(settings, kMochanAspectKey, config.mochan.aspect,
                     static_cast<int>(MochanAspectMode::kSixteenNine));
        config.mochan.render =
            ReadEnum(settings, kMochanRenderKey, config.mochan.render,
                     static_cast<int>(MochanRenderMode::kFit));
        config.mcp.resolution =
            ReadEnum(settings, kMcpResolutionKey, config.mcp.resolution,
                     static_cast<int>(CameraResolution::kUxga));
        config.mcp.jpeg_quality = settings.GetInt(kMcpQualityKey, config.mcp.jpeg_quality);
        config.mcp.freshness =
            ReadEnum(settings, kMcpFreshnessKey, config.mcp.freshness,
                     static_cast<int>(McpFreshFramePolicy::kFresh));
    }

    config = Normalize(config);
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

CameraSettingsConfig CameraSettingsStore::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void CameraSettingsStore::Save(const CameraSettingsConfig& config) {
    const CameraSettingsConfig normalized = Normalize(config);
    std::lock_guard<std::mutex> lock(mutex_);
    Persist(normalized);
    config_ = normalized;
}

void CameraSettingsStore::ResetToDefaults(bool flipped) { Save(Defaults(flipped)); }

void CameraSettingsStore::SetOrientation(bool mirror, bool flip) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.sensor.mirror = mirror;
    config_.sensor.flip = flip;
    Persist(config_);
}

void CameraSettingsStore::Persist(const CameraSettingsConfig& config) {
    Settings settings(kNamespace, true);
    settings.SetInt(kVersionKey, kSchemaVersion);
    settings.SetInt(kProfileKey, static_cast<int>(config.sensor.profile));
    settings.SetInt(kBrightnessKey, config.sensor.brightness);
    settings.SetInt(kContrastKey, config.sensor.contrast);
    settings.SetInt(kSaturationKey, config.sensor.saturation);
    settings.SetBool(kAecKey, config.sensor.auto_exposure);
    settings.SetBool(kAec2Key, config.sensor.aec2);
    settings.SetInt(kAeLevelKey, config.sensor.ae_level);
    settings.SetInt(kExposureKey, config.sensor.manual_exposure);
    settings.SetBool(kAgcKey, config.sensor.auto_gain);
    settings.SetInt(kGainKey, config.sensor.manual_gain);
    settings.SetInt(kGainCeilingKey, static_cast<int>(config.sensor.gain_ceiling));
    settings.SetBool(kAwbKey, config.sensor.auto_white_balance);
    settings.SetBool(kAwbGainKey, config.sensor.awb_gain);
    settings.SetInt(kWhiteBalanceModeKey, config.sensor.white_balance_mode);
    settings.SetBool(kBpcKey, config.sensor.black_pixel_correction);
    settings.SetBool(kWpcKey, config.sensor.white_pixel_correction);
    settings.SetBool(kGammaKey, config.sensor.gamma);
    settings.SetBool(kLensCorrectionKey, config.sensor.lens_correction);
    settings.SetBool(kMirrorKey, config.sensor.mirror);
    settings.SetBool(kFlipKey, config.sensor.flip);
    settings.SetInt(kWebResolutionKey, static_cast<int>(config.web.resolution));
    settings.SetInt(kWebQualityKey, config.web.jpeg_quality);
    settings.SetInt(kWebFpsKey, config.web.fps);
    settings.SetInt(kMochanResolutionKey, static_cast<int>(config.mochan.source_resolution));
    settings.SetInt(kMochanAspectKey, static_cast<int>(config.mochan.aspect));
    settings.SetInt(kMochanRenderKey, static_cast<int>(config.mochan.render));
    settings.SetInt(kMcpResolutionKey, static_cast<int>(config.mcp.resolution));
    settings.SetInt(kMcpQualityKey, config.mcp.jpeg_quality);
    settings.SetInt(kMcpFreshnessKey, static_cast<int>(config.mcp.freshness));
}
