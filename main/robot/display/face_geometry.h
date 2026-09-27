#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <string>

struct FaceEyeGeometry {
    int width;
    int height;
    int x;
    int y;
    int rotation;
    int top_curve;
    int bottom_curve;
    int slope;
    int water;
};

struct FaceMouthGeometry {
    int width;
    int height;
    float top_curve;
    float bottom_curve;
    float slope;
    int gap;
};

struct FaceGeometry {
    FaceEyeGeometry left_eye;
    FaceEyeGeometry right_eye;
    FaceMouthGeometry mouth;
};

class FaceGeometryStore {
public:
    static constexpr size_t kEmotionCount = 24;

    FaceGeometryStore();

    static const std::array<const char*, kEmotionCount>& EmotionNames();
    static bool GetDefault(const std::string& emotion, FaceGeometry& geometry);
    static bool Clamp(FaceGeometry& geometry);

    bool Get(const std::string& emotion, FaceGeometry& geometry, bool* customized = nullptr) const;
    bool Save(const std::string& emotion, FaceGeometry geometry);
    bool Reset(const std::string& emotion);
    bool ResetAll();

private:
    static int FindEmotion(const std::string& emotion);
    void LoadOverrides();

    mutable std::mutex mutex_;
    std::array<FaceGeometry, kEmotionCount> geometry_{};
    std::array<bool, kEmotionCount> customized_{};
};
