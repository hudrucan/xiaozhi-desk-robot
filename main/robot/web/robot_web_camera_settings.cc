#include "robot_web_camera_settings.h"

#include "control/robot_controller.h"

#include <cJSON.h>

#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace {

const char* ProfileName(CameraImageProfile value) {
    switch (value) {
        case CameraImageProfile::kLowLight: return "low_light";
        case CameraImageProfile::kCustom: return "custom";
        default: return "normal";
    }
}

const char* ResolutionName(CameraResolution value) {
    switch (value) {
        case CameraResolution::kAuto: return "auto";
        case CameraResolution::kQvga: return "qvga";
        case CameraResolution::kHvga: return "hvga";
        case CameraResolution::kVga: return "vga";
        case CameraResolution::kSvga: return "svga";
        case CameraResolution::kXga: return "xga";
        case CameraResolution::kSxga: return "sxga";
        case CameraResolution::kUxga: return "uxga";
    }
    return "vga";
}

const char* AspectName(MochanAspectMode value) {
    switch (value) {
        case MochanAspectMode::kSquare: return "1:1";
        case MochanAspectMode::kFourThree: return "4:3";
        case MochanAspectMode::kThreeTwo: return "3:2";
        case MochanAspectMode::kSixteenNine: return "16:9";
        default: return "auto";
    }
}

template <typename Enum>
bool ReadEnum(const cJSON* object, const char* key,
              std::initializer_list<std::pair<std::string_view, Enum>> values,
              Enum& output, std::string& error) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        error = std::string("Invalid ") + key;
        return false;
    }
    for (const auto& [name, value] : values) {
        if (name == item->valuestring) {
            output = value;
            return true;
        }
    }
    error = std::string("Unsupported ") + key;
    return false;
}

bool ReadInt(const cJSON* object, const char* key, int minimum, int maximum,
             int& output, std::string& error) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < minimum || item->valuedouble > maximum ||
        item->valuedouble != item->valueint) {
        error = std::string(key) + " must be an integer from " +
                std::to_string(minimum) + " to " + std::to_string(maximum);
        return false;
    }
    output = item->valueint;
    return true;
}

bool ReadBool(const cJSON* object, const char* key, bool& output, std::string& error) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsBool(item)) {
        error = std::string("Invalid ") + key;
        return false;
    }
    output = cJSON_IsTrue(item);
    return true;
}

cJSON* AddSettings(cJSON* root, const CameraSettingsConfig& settings) {
    cJSON* sensor = cJSON_AddObjectToObject(root, "sensor_settings");
    cJSON_AddStringToObject(sensor, "profile", ProfileName(settings.sensor.profile));
    cJSON_AddNumberToObject(sensor, "brightness", settings.sensor.brightness);
    cJSON_AddNumberToObject(sensor, "contrast", settings.sensor.contrast);
    cJSON_AddNumberToObject(sensor, "saturation", settings.sensor.saturation);
    cJSON_AddBoolToObject(sensor, "auto_exposure", settings.sensor.auto_exposure);
    cJSON_AddBoolToObject(sensor, "aec2", settings.sensor.aec2);
    cJSON_AddNumberToObject(sensor, "ae_level", settings.sensor.ae_level);
    cJSON_AddNumberToObject(sensor, "manual_exposure", settings.sensor.manual_exposure);
    cJSON_AddBoolToObject(sensor, "auto_gain", settings.sensor.auto_gain);
    cJSON_AddNumberToObject(sensor, "manual_gain", settings.sensor.manual_gain);
    cJSON_AddNumberToObject(sensor, "gain_ceiling",
                            2 << static_cast<int>(settings.sensor.gain_ceiling));
    cJSON_AddBoolToObject(sensor, "auto_white_balance", settings.sensor.auto_white_balance);
    cJSON_AddBoolToObject(sensor, "awb_gain", settings.sensor.awb_gain);
    cJSON_AddNumberToObject(sensor, "white_balance_mode", settings.sensor.white_balance_mode);
    cJSON_AddBoolToObject(sensor, "black_pixel_correction",
                          settings.sensor.black_pixel_correction);
    cJSON_AddBoolToObject(sensor, "white_pixel_correction",
                          settings.sensor.white_pixel_correction);
    cJSON_AddBoolToObject(sensor, "gamma", settings.sensor.gamma);
    cJSON_AddBoolToObject(sensor, "lens_correction", settings.sensor.lens_correction);
    cJSON_AddBoolToObject(sensor, "mirror", settings.sensor.mirror);
    cJSON_AddBoolToObject(sensor, "flip", settings.sensor.flip);

    cJSON* web = cJSON_AddObjectToObject(root, "web");
    cJSON_AddStringToObject(web, "resolution", ResolutionName(settings.web.resolution));
    cJSON_AddNumberToObject(web, "jpeg_quality", settings.web.jpeg_quality);
    cJSON_AddNumberToObject(web, "fps", settings.web.fps);

    cJSON* mochan = cJSON_AddObjectToObject(root, "mochan");
    cJSON_AddStringToObject(mochan, "resolution",
                            ResolutionName(settings.mochan.source_resolution));
    cJSON_AddStringToObject(mochan, "aspect", AspectName(settings.mochan.aspect));
    cJSON_AddStringToObject(mochan, "render",
                            settings.mochan.render == MochanRenderMode::kFit ? "fit" : "fill_crop");

    cJSON* mcp = cJSON_AddObjectToObject(root, "mcp");
    cJSON_AddStringToObject(mcp, "resolution", ResolutionName(settings.mcp.resolution));
    cJSON_AddNumberToObject(mcp, "jpeg_quality", settings.mcp.jpeg_quality);
    cJSON_AddStringToObject(mcp, "freshness",
                            settings.mcp.freshness == McpFreshFramePolicy::kLatest
                                ? "latest" : "fresh");
    return root;
}

}  // namespace

std::string RobotWebCameraSettings::Encode(bool ok, const char* message) const {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return R"({"ok":false,"message":"Out of memory"})";
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    if (message != nullptr) {
        cJSON_AddStringToObject(root, "message", message);
    }
    cJSON_AddStringToObject(root, "sensor", controller_.GetCameraSensorName().c_str());
    cJSON_AddStringToObject(root, "max_resolution", "uxga");
    cJSON_AddBoolToObject(root, "supports_jpeg", true);
    AddSettings(root, controller_.GetCameraSettings());
    char* encoded = cJSON_PrintUnformatted(root);
    std::string result = encoded != nullptr ? encoded : R"({"ok":false})";
    cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}

bool RobotWebCameraSettings::Decode(const char* body, size_t length,
                                    CameraSettingsConfig& settings,
                                    std::string& error) const {
    cJSON* root = cJSON_ParseWithLength(body, length);
    if (root == nullptr) {
        error = "Invalid JSON";
        return false;
    }
    settings = controller_.GetCameraSettings();
    const cJSON* sensor = cJSON_GetObjectItemCaseSensitive(root, "sensor_settings");
    const cJSON* web = cJSON_GetObjectItemCaseSensitive(root, "web");
    const cJSON* mochan = cJSON_GetObjectItemCaseSensitive(root, "mochan");
    const cJSON* mcp = cJSON_GetObjectItemCaseSensitive(root, "mcp");
    bool valid = cJSON_IsObject(sensor) && cJSON_IsObject(web) && cJSON_IsObject(mochan) &&
                 cJSON_IsObject(mcp);
    if (!valid) {
        error = "Missing camera settings group";
    }

    if (valid) valid = ReadEnum(sensor, "profile", {{"normal", CameraImageProfile::kNormal},
        {"low_light", CameraImageProfile::kLowLight}, {"custom", CameraImageProfile::kCustom}},
        settings.sensor.profile, error);
    if (valid) valid = ReadInt(sensor, "brightness", -2, 2, settings.sensor.brightness, error);
    if (valid) valid = ReadInt(sensor, "contrast", -2, 2, settings.sensor.contrast, error);
    if (valid) valid = ReadInt(sensor, "saturation", -2, 2, settings.sensor.saturation, error);
    if (valid) valid = ReadBool(sensor, "auto_exposure", settings.sensor.auto_exposure, error);
    if (valid) valid = ReadBool(sensor, "aec2", settings.sensor.aec2, error);
    if (valid) valid = ReadInt(sensor, "ae_level", -2, 2, settings.sensor.ae_level, error);
    if (valid) valid = ReadInt(sensor, "manual_exposure", 0, 1200,
                               settings.sensor.manual_exposure, error);
    if (valid) valid = ReadBool(sensor, "auto_gain", settings.sensor.auto_gain, error);
    if (valid) valid = ReadInt(sensor, "manual_gain", 0, 30, settings.sensor.manual_gain, error);
    int gain_ceiling = 2;
    if (valid) valid = ReadInt(sensor, "gain_ceiling", 2, 128, gain_ceiling, error);
    if (valid && (gain_ceiling & (gain_ceiling - 1)) != 0) {
        error = "Invalid gain_ceiling";
        valid = false;
    }
    if (valid) {
        int index = 0;
        for (int value = gain_ceiling; value > 2; value >>= 1) ++index;
        settings.sensor.gain_ceiling = static_cast<CameraGainCeiling>(index);
    }
    if (valid) valid = ReadBool(sensor, "auto_white_balance",
                                settings.sensor.auto_white_balance, error);
    if (valid) valid = ReadBool(sensor, "awb_gain", settings.sensor.awb_gain, error);
    if (valid) valid = ReadInt(sensor, "white_balance_mode", 0, 4,
                               settings.sensor.white_balance_mode, error);
    if (valid) valid = ReadBool(sensor, "black_pixel_correction",
                                settings.sensor.black_pixel_correction, error);
    if (valid) valid = ReadBool(sensor, "white_pixel_correction",
                                settings.sensor.white_pixel_correction, error);
    if (valid) valid = ReadBool(sensor, "gamma", settings.sensor.gamma, error);
    if (valid) valid = ReadBool(sensor, "lens_correction", settings.sensor.lens_correction, error);
    if (valid) valid = ReadBool(sensor, "mirror", settings.sensor.mirror, error);
    if (valid) valid = ReadBool(sensor, "flip", settings.sensor.flip, error);

    const auto resolutions = std::initializer_list<std::pair<std::string_view, CameraResolution>>{
        {"auto", CameraResolution::kAuto}, {"qvga", CameraResolution::kQvga},
        {"hvga", CameraResolution::kHvga}, {"vga", CameraResolution::kVga},
        {"svga", CameraResolution::kSvga}, {"xga", CameraResolution::kXga},
        {"sxga", CameraResolution::kSxga}, {"uxga", CameraResolution::kUxga}};
    if (valid) valid = ReadEnum(web, "resolution", resolutions, settings.web.resolution, error);
    if (valid) valid = ReadInt(web, "jpeg_quality", 4, 63, settings.web.jpeg_quality, error);
    if (valid) valid = ReadInt(web, "fps", 1, 10, settings.web.fps, error);
    if (valid) valid = ReadEnum(mochan, "resolution", resolutions,
                                settings.mochan.source_resolution, error);
    if (valid) valid = ReadEnum(mochan, "aspect", {{"auto", MochanAspectMode::kAuto},
        {"1:1", MochanAspectMode::kSquare}, {"4:3", MochanAspectMode::kFourThree},
        {"3:2", MochanAspectMode::kThreeTwo}, {"16:9", MochanAspectMode::kSixteenNine}},
        settings.mochan.aspect, error);
    if (valid) valid = ReadEnum(mochan, "render", {{"fill_crop", MochanRenderMode::kFillCrop},
        {"fit", MochanRenderMode::kFit}}, settings.mochan.render, error);
    if (valid) valid = ReadEnum(mcp, "resolution", resolutions, settings.mcp.resolution, error);
    if (valid) valid = ReadInt(mcp, "jpeg_quality", 4, 63, settings.mcp.jpeg_quality, error);
    if (valid) valid = ReadEnum(mcp, "freshness", {{"latest", McpFreshFramePolicy::kLatest},
        {"fresh", McpFreshFramePolicy::kFresh}}, settings.mcp.freshness, error);

    cJSON_Delete(root);
    if (!valid) {
        return false;
    }
    const CameraSettingsConfig normalized = CameraSettingsStore::Normalize(settings);
    if (normalized.web.resolution != settings.web.resolution ||
        normalized.mochan.source_resolution != settings.mochan.source_resolution ||
        normalized.mcp.resolution != settings.mcp.resolution) {
        error = "Resolution is not supported for that camera mode";
        return false;
    }
    settings = normalized;
    return true;
}
