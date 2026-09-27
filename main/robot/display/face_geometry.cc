#include "face_geometry.h"

#include <nvs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

constexpr char kNamespace[] = "face_geometry";
constexpr uint32_t kRecordMagic = 0x4647454f;  // FGEO
constexpr uint16_t kRecordVersion = 1;

struct CompiledFaceGeometry {
    const char* emotion;
    FaceGeometry geometry;
};

constexpr std::array<CompiledFaceGeometry, FaceGeometryStore::kEmotionCount> kDefaults = {{
    {"neutral", {{74,54,-48,-59,0,3,0,0,0},{74,54,48,-59,0,3,0,0,0},{72,24,2,2,0,74}}},
    {"happy", {{76,50,-47,-56,0,3,-6,0,0},{76,50,47,-56,0,3,-6,0,0},{80,20,2.5f,12,0,64}}},
    {"bored", {{78,33,-46,-55,0,0,0,-3,0},{78,33,46,-55,0,0,0,3,0},{76,22,0,0,-1.5f,72}}},
    {"laughing", {{76,38,-47,-54,0,-9,-19,-2,0},{76,38,47,-54,0,-9,-19,2,0},{84,36,4,20,0,62}}},
    {"funny", {{54,61,-48,-61,-60,0,-3,0,0},{72,40,48,-54,40,-5,-18,0,0},{68,26,-3,6,-4,68}}},
    {"sad", {{72,49,-47,-55,0,10,0,-9,0},{72,49,47,-55,0,10,0,9,0},{68,20,-6,-4,0,66}}},
    {"angry", {{74,43,-45,-55,0,2,0,12,0},{74,43,45,-55,0,2,0,-12,0},{78,24,-10,-6,0,74}}},
    {"crying", {{72,49,-47,-55,0,10,0,-9,18},{72,49,47,-55,0,10,0,9,18},{62,28,-7.5f,6,0,68}}},
    {"loving", {{62,48,-40,-57,60,-4,-12,2,0},{62,48,40,-57,-60,-4,-12,-2,0},{54,20,2,9,0,62}}},
    {"embarrassed", {{59,38,-51,-47,0,8,-3,-5,0},{59,38,41,-47,0,8,-3,5,0},{56,16,-2,1,2,64}}},
    {"surprised", {{49,65,-44,-57,0,0,0,0,0},{49,65,44,-57,0,0,0,0,0},{54,38,-3,10,0,78}}},
    {"shocked", {{59,69,-44,-57,0,0,0,0,0},{49,72,44,-59,0,0,0,0,0},{52,48,-4,12,0,76}}},
    {"thinking", {{60,54,-53,-63,0,3,0,-5,0},{68,34,45,-53,0,8,0,3,0},{52,18,1,0,3,66}}},
    {"winking", {{69,30,-47,-53,0,-6,-15,0,0},{70,54,47,-58,0,0,-5,0,0},{74,22,1.5f,8,3.5f,64}}},
    {"cool", {{78,33,-46,-55,0,0,0,-3,0},{78,33,46,-55,0,0,0,3,0},{66,16,0,2,2,68}}},
    {"relaxed", {{71,40,-47,-54,0,4,-6,0,0},{71,40,47,-54,0,4,-6,0,0},{68,18,1.5f,5,0,68}}},
    {"delicious", {{69,41,-45,-54,0,-4,-15,0,0},{69,41,45,-54,0,-4,-15,0,0},{74,26,2.5f,14,0,64}}},
    {"kissy", {{53,33,-37,-54,-80,-3,-12,0,0},{53,33,37,-54,80,-3,-12,0,0},{36,28,-2,3,0,62}}},
    {"confident", {{70,51,-46,-61,0,-3,0,3,0},{73,34,46,-53,0,4,-2,-4,0},{72,22,1,6,3,66}}},
    {"sleepy", {{72,26,-46,-48,0,7,1,0,0},{72,26,46,-48,0,7,1,0,0},{56,28,-6,6,0,70}}},
    {"silly", {{48,62,-48,-64,80,0,0,-3,0},{77,33,48,-46,-80,2,-8,0,0},{70,28,-2,12,-4,66}}},
    {"confused", {{69,33,-49,-49,0,8,0,-7,0},{54,57,48,-63,0,-3,0,3,0},{58,20,-2,2,3.5f,66}}},
    {"suspicious", {{74,31,-40,-52,0,6,0,3,0},{63,43,54,-59,0,9,0,-3,0},{64,16,-1,-1,-1.5f,68}}},
    {"shake", {{72,46,-48,-57,0,0,0,0,0},{72,46,48,-57,0,0,0,0,0},{66,22,0,4,0,70}}},
}};

struct PersistedGeometry {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int16_t values[24];
    uint32_t checksum;
};

uint32_t Checksum(const PersistedGeometry& record) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(PersistedGeometry, checksum); ++i) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

PersistedGeometry Encode(const FaceGeometry& geometry) {
    PersistedGeometry record{};
    record.magic = kRecordMagic;
    record.version = kRecordVersion;
    int index = 0;
    for (const auto* eye : {&geometry.left_eye, &geometry.right_eye}) {
        record.values[index++] = eye->width;
        record.values[index++] = eye->height;
        record.values[index++] = eye->x;
        record.values[index++] = eye->y;
        record.values[index++] = eye->rotation;
        record.values[index++] = eye->top_curve;
        record.values[index++] = eye->bottom_curve;
        record.values[index++] = eye->slope;
        record.values[index++] = eye->water;
    }
    record.values[index++] = geometry.mouth.width;
    record.values[index++] = geometry.mouth.height;
    record.values[index++] = std::lround(geometry.mouth.top_curve * 10.0f);
    record.values[index++] = std::lround(geometry.mouth.bottom_curve * 10.0f);
    record.values[index++] = std::lround(geometry.mouth.slope * 10.0f);
    record.values[index] = geometry.mouth.gap;
    record.checksum = Checksum(record);
    return record;
}

bool Decode(const PersistedGeometry& record, FaceGeometry& geometry) {
    if (record.magic != kRecordMagic || record.version != kRecordVersion ||
        record.checksum != Checksum(record)) {
        return false;
    }
    int index = 0;
    for (auto* eye : {&geometry.left_eye, &geometry.right_eye}) {
        eye->width = record.values[index++];
        eye->height = record.values[index++];
        eye->x = record.values[index++];
        eye->y = record.values[index++];
        eye->rotation = record.values[index++];
        eye->top_curve = record.values[index++];
        eye->bottom_curve = record.values[index++];
        eye->slope = record.values[index++];
        eye->water = record.values[index++];
    }
    geometry.mouth.width = record.values[index++];
    geometry.mouth.height = record.values[index++];
    geometry.mouth.top_curve = record.values[index++] / 10.0f;
    geometry.mouth.bottom_curve = record.values[index++] / 10.0f;
    geometry.mouth.slope = record.values[index++] / 10.0f;
    geometry.mouth.gap = record.values[index];
    FaceGeometryStore::Clamp(geometry);
    return true;
}

}  // namespace

FaceGeometryStore::FaceGeometryStore() {
    for (size_t i = 0; i < kDefaults.size(); ++i) {
        geometry_[i] = kDefaults[i].geometry;
    }
    LoadOverrides();
}

const std::array<const char*, FaceGeometryStore::kEmotionCount>&
FaceGeometryStore::EmotionNames() {
    static const std::array<const char*, kEmotionCount> names = [] {
        std::array<const char*, kEmotionCount> result{};
        for (size_t i = 0; i < kDefaults.size(); ++i) result[i] = kDefaults[i].emotion;
        return result;
    }();
    return names;
}

int FaceGeometryStore::FindEmotion(const std::string& emotion) {
    for (size_t i = 0; i < kDefaults.size(); ++i) {
        if (emotion == kDefaults[i].emotion) return static_cast<int>(i);
    }
    return -1;
}

bool FaceGeometryStore::GetDefault(const std::string& emotion, FaceGeometry& geometry) {
    const int index = FindEmotion(emotion);
    if (index < 0) return false;
    geometry = kDefaults[index].geometry;
    return true;
}

bool FaceGeometryStore::Clamp(FaceGeometry& geometry) {
    const FaceGeometry original = geometry;
    for (auto* eye : {&geometry.left_eye, &geometry.right_eye}) {
        eye->width = std::clamp(eye->width, 8, 96);
        eye->height = std::clamp(eye->height, 7, 88);
        eye->x = std::clamp(eye->x, -100, 100);
        eye->y = std::clamp(eye->y, -110, 80);
        eye->rotation = std::clamp(eye->rotation, -1800, 1800);
        eye->top_curve = std::clamp(eye->top_curve, -44, 44);
        eye->bottom_curve = std::clamp(eye->bottom_curve, -44, 44);
        eye->slope = std::clamp(eye->slope, -44, 44);
        eye->water = std::clamp(eye->water, 0, 40);
    }
    geometry.mouth.width = std::clamp(geometry.mouth.width, 8, 120);
    geometry.mouth.height = std::clamp(geometry.mouth.height, 7, 80);
    geometry.mouth.top_curve =
        std::round(std::clamp(geometry.mouth.top_curve, -40.0f, 40.0f) * 10.0f) / 10.0f;
    geometry.mouth.bottom_curve =
        std::round(std::clamp(geometry.mouth.bottom_curve, -40.0f, 40.0f) * 10.0f) / 10.0f;
    geometry.mouth.slope =
        std::round(std::clamp(geometry.mouth.slope, -40.0f, 40.0f) * 10.0f) / 10.0f;
    geometry.mouth.gap = std::clamp(geometry.mouth.gap, 20, 110);
    const auto same_eye = [](const FaceEyeGeometry& first, const FaceEyeGeometry& second) {
        return first.width == second.width && first.height == second.height &&
               first.x == second.x && first.y == second.y &&
               first.rotation == second.rotation && first.top_curve == second.top_curve &&
               first.bottom_curve == second.bottom_curve && first.slope == second.slope &&
               first.water == second.water;
    };
    return !same_eye(original.left_eye, geometry.left_eye) ||
           !same_eye(original.right_eye, geometry.right_eye) ||
           original.mouth.width != geometry.mouth.width ||
           original.mouth.height != geometry.mouth.height ||
           original.mouth.top_curve != geometry.mouth.top_curve ||
           original.mouth.bottom_curve != geometry.mouth.bottom_curve ||
           original.mouth.slope != geometry.mouth.slope ||
           original.mouth.gap != geometry.mouth.gap;
}

bool FaceGeometryStore::Get(const std::string& emotion, FaceGeometry& geometry,
                            bool* customized) const {
    const int index = FindEmotion(emotion);
    if (index < 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    geometry = geometry_[index];
    if (customized != nullptr) *customized = customized_[index];
    return true;
}

bool FaceGeometryStore::Save(const std::string& emotion, FaceGeometry geometry) {
    const int index = FindEmotion(emotion);
    if (index < 0) return false;
    Clamp(geometry);
    const PersistedGeometry record = Encode(geometry);
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t written = nvs_set_blob(handle, emotion.c_str(), &record, sizeof(record));
    const esp_err_t committed = written == ESP_OK ? nvs_commit(handle) : written;
    nvs_close(handle);
    if (written != ESP_OK || committed != ESP_OK) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    geometry_[index] = geometry;
    customized_[index] = true;
    return true;
}

bool FaceGeometryStore::Reset(const std::string& emotion) {
    const int index = FindEmotion(emotion);
    if (index < 0) return false;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t erased = nvs_erase_key(handle, emotion.c_str());
    if (erased == ESP_ERR_NVS_NOT_FOUND) erased = ESP_OK;
    const esp_err_t committed = erased == ESP_OK ? nvs_commit(handle) : erased;
    nvs_close(handle);
    if (erased != ESP_OK || committed != ESP_OK) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    geometry_[index] = kDefaults[index].geometry;
    customized_[index] = false;
    return true;
}

bool FaceGeometryStore::ResetAll() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t erased = nvs_erase_all(handle);
    const esp_err_t committed = erased == ESP_OK ? nvs_commit(handle) : erased;
    nvs_close(handle);
    if (erased != ESP_OK || committed != ESP_OK) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < kDefaults.size(); ++i) {
        geometry_[i] = kDefaults[i].geometry;
        customized_[i] = false;
    }
    return true;
}

void FaceGeometryStore::LoadOverrides() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    for (size_t i = 0; i < kDefaults.size(); ++i) {
        PersistedGeometry record{};
        size_t size = sizeof(record);
        if (nvs_get_blob(handle, kDefaults[i].emotion, &record, &size) == ESP_OK &&
            size == sizeof(record) && Decode(record, geometry_[i])) {
            customized_[i] = true;
        }
    }
    nvs_close(handle);
}
