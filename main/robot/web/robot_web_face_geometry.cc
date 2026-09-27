#include "robot_web_face_geometry.h"

#include "control/robot_controller.h"

#include <cJSON.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

void AddEye(cJSON* root, const char* name, const FaceEyeGeometry& eye) {
    cJSON* object = cJSON_AddObjectToObject(root, name);
    cJSON_AddNumberToObject(object, "width", eye.width);
    cJSON_AddNumberToObject(object, "height", eye.height);
    cJSON_AddNumberToObject(object, "x", eye.x);
    cJSON_AddNumberToObject(object, "y", eye.y);
    cJSON_AddNumberToObject(object, "rotation", eye.rotation);
    cJSON_AddNumberToObject(object, "top_curve", eye.top_curve);
    cJSON_AddNumberToObject(object, "bottom_curve", eye.bottom_curve);
    cJSON_AddNumberToObject(object, "slope", eye.slope);
    cJSON_AddNumberToObject(object, "water", eye.water);
}

void AddGeometry(cJSON* root, const char* name, const FaceGeometry& geometry) {
    cJSON* object = cJSON_AddObjectToObject(root, name);
    AddEye(object, "left_eye", geometry.left_eye);
    AddEye(object, "right_eye", geometry.right_eye);
    cJSON* mouth = cJSON_AddObjectToObject(object, "mouth");
    cJSON_AddNumberToObject(mouth, "width", geometry.mouth.width);
    cJSON_AddNumberToObject(mouth, "height", geometry.mouth.height);
    cJSON_AddNumberToObject(mouth, "top_curve", geometry.mouth.top_curve);
    cJSON_AddNumberToObject(mouth, "bottom_curve", geometry.mouth.bottom_curve);
    cJSON_AddNumberToObject(mouth, "slope", geometry.mouth.slope);
    cJSON_AddNumberToObject(mouth, "gap", geometry.mouth.gap);
}

bool ReadInteger(const cJSON* object, const char* key, int& output, std::string& error) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::trunc(item->valuedouble) != item->valuedouble ||
        item->valuedouble < std::numeric_limits<int16_t>::min() ||
        item->valuedouble > std::numeric_limits<int16_t>::max()) {
        error = std::string("Invalid ") + key;
        return false;
    }
    output = static_cast<int>(item->valuedouble);
    return true;
}

bool ReadFloat(const cJSON* object, const char* key, float& output, std::string& error) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        item->valuedouble < -1000.0 || item->valuedouble > 1000.0) {
        error = std::string("Invalid ") + key;
        return false;
    }
    output = static_cast<float>(item->valuedouble);
    return true;
}

bool ReadEye(const cJSON* root, const char* name, FaceEyeGeometry& eye, std::string& error) {
    const cJSON* object = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsObject(object)) {
        error = std::string("Missing ") + name;
        return false;
    }
    return ReadInteger(object, "width", eye.width, error) &&
           ReadInteger(object, "height", eye.height, error) &&
           ReadInteger(object, "x", eye.x, error) &&
           ReadInteger(object, "y", eye.y, error) &&
           ReadInteger(object, "rotation", eye.rotation, error) &&
           ReadInteger(object, "top_curve", eye.top_curve, error) &&
           ReadInteger(object, "bottom_curve", eye.bottom_curve, error) &&
           ReadInteger(object, "slope", eye.slope, error) &&
           ReadInteger(object, "water", eye.water, error);
}

}  // namespace

std::string RobotWebFaceGeometry::Encode(const std::string& emotion, bool ok,
                                         const char* message) const {
    FaceGeometry geometry{};
    FaceGeometry defaults{};
    bool customized = false;
    int layout_y_offset = 0;
    if (!controller_.GetFaceGeometry(emotion, geometry, &customized) ||
        !FaceGeometryStore::GetDefault(emotion, defaults) ||
        !controller_.GetFaceGeometryLayoutYOffset(emotion, layout_y_offset)) {
        ok = false;
        message = "Unsupported emotion";
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return R"({"ok":false,"message":"Out of memory"})";
    cJSON_AddBoolToObject(root, "ok", ok);
    if (message != nullptr) cJSON_AddStringToObject(root, "message", message);
    cJSON_AddStringToObject(root, "emotion", emotion.c_str());
    cJSON_AddStringToObject(root, "state", customized ? "customized" : "default");
    cJSON_AddBoolToObject(root, "customized", customized);
    if (ok) {
        cJSON* metadata = cJSON_AddObjectToObject(root, "metadata");
        cJSON_AddNumberToObject(metadata, "layout_y_offset", layout_y_offset);
        AddGeometry(root, "geometry", geometry);
        AddGeometry(root, "default", defaults);
    }
    char* encoded = cJSON_PrintUnformatted(root);
    std::string result = encoded != nullptr ? encoded : R"({"ok":false})";
    cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}

bool RobotWebFaceGeometry::Decode(const char* body, size_t length, std::string& emotion,
                                  FaceGeometry& geometry, std::string& error) const {
    cJSON* root = cJSON_ParseWithLength(body, length);
    if (root == nullptr) {
        error = "Invalid JSON";
        return false;
    }
    const cJSON* emotion_item = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    const cJSON* source = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    bool valid = cJSON_IsString(emotion_item) && emotion_item->valuestring != nullptr &&
                 cJSON_IsObject(source);
    if (!valid) {
        error = "Missing emotion or geometry";
    } else {
        emotion = emotion_item->valuestring;
        FaceGeometry defaults{};
        valid = FaceGeometryStore::GetDefault(emotion, defaults);
        if (!valid) error = "Unsupported emotion";
    }
    if (valid) valid = ReadEye(source, "left_eye", geometry.left_eye, error);
    if (valid) valid = ReadEye(source, "right_eye", geometry.right_eye, error);
    const cJSON* mouth = valid ? cJSON_GetObjectItemCaseSensitive(source, "mouth") : nullptr;
    if (valid && !cJSON_IsObject(mouth)) {
        error = "Missing mouth";
        valid = false;
    }
    if (valid) valid = ReadInteger(mouth, "width", geometry.mouth.width, error);
    if (valid) valid = ReadInteger(mouth, "height", geometry.mouth.height, error);
    if (valid) valid = ReadFloat(mouth, "top_curve", geometry.mouth.top_curve, error);
    if (valid) valid = ReadFloat(mouth, "bottom_curve", geometry.mouth.bottom_curve, error);
    if (valid) valid = ReadFloat(mouth, "slope", geometry.mouth.slope, error);
    if (valid) valid = ReadInteger(mouth, "gap", geometry.mouth.gap, error);
    cJSON_Delete(root);
    if (valid) FaceGeometryStore::Clamp(geometry);
    return valid;
}
