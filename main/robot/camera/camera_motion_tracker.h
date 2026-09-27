#pragma once

#include <cstddef>
#include <cstdint>

enum class CameraMotionHorizontalRegion : uint8_t {
    kNone,
    kLeft,
    kCenter,
    kRight,
};

enum class CameraMotionVerticalRegion : uint8_t {
    kNone,
    kTop,
    kCenter,
    kBottom,
};

struct CameraMotionSpatialMetrics {
    bool valid = false;
    uint16_t active_cells = 0;
    float centroid_x = 0.0f;
    float centroid_y = 0.0f;
    float bbox_left = 0.0f;
    float bbox_top = 0.0f;
    float bbox_right = 0.0f;
    float bbox_bottom = 0.0f;
    float bbox_area_ratio = 0.0f;
    CameraMotionHorizontalRegion horizontal_region =
        CameraMotionHorizontalRegion::kNone;
    CameraMotionVerticalRegion vertical_region =
        CameraMotionVerticalRegion::kNone;
};

class CameraMotionTracker {
public:
    void BeginFrame(size_t width, size_t height, float threshold);
    void AddCell(size_t x, size_t y, float difference);
    CameraMotionSpatialMetrics FinishFrame() const;

    static const char* HorizontalRegionName(CameraMotionHorizontalRegion region);
    static const char* VerticalRegionName(CameraMotionVerticalRegion region);

private:
    size_t width_ = 0;
    size_t height_ = 0;
    float threshold_ = 0.0f;
    uint16_t active_cells_ = 0;
    size_t min_x_ = 0;
    size_t min_y_ = 0;
    size_t max_x_ = 0;
    size_t max_y_ = 0;
    float total_weight_ = 0.0f;
    float weighted_x_ = 0.0f;
    float weighted_y_ = 0.0f;
};
