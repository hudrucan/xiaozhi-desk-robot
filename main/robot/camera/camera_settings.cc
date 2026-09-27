#include "camera_settings.h"

#include "settings.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace {

constexpr char kNamespace[] = "camera";
constexpr int kSchemaVersion = 6;
constexpr int kFirstCompatibleSchemaVersion = 1;
constexpr int kOrientationSchemaVersion = 3;

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
constexpr char kAdvancedAwbKey[] = "adv_awb";
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
constexpr char kVisionEnabledKey[] = "vision_en";
constexpr char kVisionIntervalKey[] = "vision_ms";
constexpr char kFaceDetectionKey[] = "face_det";

constexpr size_t kNvsNameMaxLength = 15;
constexpr std::array<std::string_view, 34> kNvsNames = {
    kNamespace,          kVersionKey,          kProfileKey,        kBrightnessKey,
    kContrastKey,        kSaturationKey,       kAecKey,            kAec2Key,
    kAeLevelKey,         kExposureKey,         kAgcKey,            kGainKey,
    kGainCeilingKey,     kAwbKey,              kAwbGainKey,        kAdvancedAwbKey,
    kWhiteBalanceModeKey, kBpcKey,             kWpcKey,            kGammaKey,
    kLensCorrectionKey,  kMirrorKey,           kFlipKey,           kWebResolutionKey,
    kWebQualityKey,      kWebFpsKey,           kMochanResolutionKey, kMochanAspectKey,
    kMochanRenderKey,    kMcpResolutionKey,    kMcpQualityKey,       kVisionEnabledKey,
    kVisionIntervalKey,  kFaceDetectionKey,
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
                                CameraResolution::kUxga, CameraResolution::kQsxga})
               ? resolution
               : CameraResolution::kUxga;
}

int NormalizeVisionInterval(int interval_ms) {
    return interval_ms == 500 || interval_ms == 1000 || interval_ms == 2000
               ? interval_ms
               : 1000;
}

}  // namespace

CameraSettingsConfig CameraSettingsStore::Defaults() { return {}; }

CameraSettingsConfig CameraSettingsStore::Normalize(CameraSettingsConfig config) {
    config.sensor.profile =
        NormalizeEnum(config.sensor.profile, CameraImageProfile::kAuto,
                      CameraImageProfile::kNormal);
    config.sensor.brightness = std::clamp(config.sensor.brightness, -3, 3);
    config.sensor.contrast = std::clamp(config.sensor.contrast, -3, 3);
    config.sensor.saturation = std::clamp(config.sensor.saturation, -4, 4);
    config.sensor.ae_level = std::clamp(config.sensor.ae_level, -5, 5);
    config.sensor.manual_exposure = std::clamp(config.sensor.manual_exposure, 0, 1200);
    config.sensor.manual_gain = std::clamp(config.sensor.manual_gain, 0, 64);
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
    config.vision.interval_ms = NormalizeVisionInterval(config.vision.interval_ms);
    return config;
}

void CameraSettingsStore::Load() {
    Settings settings(kNamespace);
    CameraSettingsConfig config = Defaults();
    const int stored_schema = settings.GetInt(kVersionKey, 0);
    const bool has_compatible_schema =
        stored_schema >= kFirstCompatibleSchemaVersion && stored_schema <= kSchemaVersion;
    if (has_compatible_schema) {
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
        config.sensor.advanced_awb =
            settings.GetBool(kAdvancedAwbKey, config.sensor.advanced_awb);
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
                     static_cast<int>(CameraResolution::kQsxga));
        config.web.jpeg_quality = settings.GetInt(kWebQualityKey, config.web.jpeg_quality);
        config.web.fps = settings.GetInt(kWebFpsKey, config.web.fps);
        config.mochan.source_resolution =
            ReadEnum(settings, kMochanResolutionKey, config.mochan.source_resolution,
                     static_cast<int>(CameraResolution::kQsxga));
        config.mochan.aspect =
            ReadEnum(settings, kMochanAspectKey, config.mochan.aspect,
                     static_cast<int>(MochanAspectMode::kSixteenNine));
        config.mochan.render =
            ReadEnum(settings, kMochanRenderKey, config.mochan.render,
                     static_cast<int>(MochanRenderMode::kFit));
        config.mcp.resolution =
            ReadEnum(settings, kMcpResolutionKey, config.mcp.resolution,
                     static_cast<int>(CameraResolution::kQsxga));
        config.mcp.jpeg_quality = settings.GetInt(kMcpQualityKey, config.mcp.jpeg_quality);
        config.mcp.freshness =
            ReadEnum(settings, kMcpFreshnessKey, config.mcp.freshness,
                     static_cast<int>(McpFreshFramePolicy::kFresh));
        config.vision.enabled =
            settings.GetBool(kVisionEnabledKey, config.vision.enabled);
        config.vision.interval_ms =
            settings.GetInt(kVisionIntervalKey, config.vision.interval_ms);
        config.vision.face_detection_enabled =
            settings.GetBool(kFaceDetectionKey,
                             config.vision.face_detection_enabled);
    }

    const bool migrate_ov5640_orientation =
        stored_schema < kOrientationSchemaVersion;
    if (migrate_ov5640_orientation) {
        // Schemas v0-v2 predate the confirmed physical OV5640 baseline.
        // Preserve compatible image/capture settings while migrating both
        // orientation axes once. Schema v3 user choices are never overwritten.
        config.sensor.mirror = kDefaultCameraMirror;
        config.sensor.flip = kDefaultCameraFlip;
    }

    config = Normalize(config);
    // Schema upgrades add defaults only. Re-persist compatible older settings
    // without re-running the fixed v3 orientation migration.
    const bool migrate_compatible_schema =
        stored_schema >= kFirstCompatibleSchemaVersion && stored_schema < kSchemaVersion;
    if (migrate_ov5640_orientation || migrate_compatible_schema) {
        Persist(config);
    }
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

void CameraSettingsStore::ResetToDefaults() { Save(Defaults()); }

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
    settings.SetBool(kAdvancedAwbKey, config.sensor.advanced_awb);
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
    settings.SetBool(kVisionEnabledKey, config.vision.enabled);
    settings.SetInt(kVisionIntervalKey, config.vision.interval_ms);
    settings.SetBool(kFaceDetectionKey, config.vision.face_detection_enabled);
}
