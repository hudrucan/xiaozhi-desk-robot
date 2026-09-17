#include "mochan_display.h"

#include "src/misc/cache/instance/lv_image_cache.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr int kEyeLayoutOffsetY = 10;
constexpr int kMouthEyeCenterGap = 65;
constexpr int kSurprisedMouthExtraGap = 10;
constexpr char kTag[] = "MochanDisplay";

struct MouthPoint {
    int8_t x;
    int8_t y;
};

struct MouthGeometry {
    const char* emotion;
    int x;
    int y;
    int width;
    int height;
    int idle_eye_offset_y;
    int base_scale_y;
    int blink_scale_y;
    int idle_open_scale_y;
    const MouthPoint* points;
    size_t point_count;
};

constexpr MouthPoint kNeutralMouth[] = {
    // Keep the eyes' full rounded-block silhouette, with a relaxed two-pixel
    // dip at the lip and a soft lower contour instead of a rigid flat bar.
    {12, 0}, {22, 1}, {32, 2}, {44, 2}, {54, 1}, {64, 0},
    {69, 1}, {73, 4}, {75, 8}, {76, 13}, {75, 18}, {73, 22},
    {69, 25}, {63, 27}, {54, 29}, {44, 30}, {32, 30}, {22, 29},
    {13, 27}, {7, 25}, {3, 22}, {1, 18}, {0, 13}, {1, 8},
    {3, 4}, {7, 1},
};
constexpr MouthPoint kHappyMouth[] = {
    // A full, softly opened smile: rounded cheeks instead of sharp raised
    // tips, and a shallow upper curve instead of a thin crescent/grin.
    {10, 3}, {20, 5}, {32, 7}, {42, 8}, {52, 7}, {64, 5}, {74, 3},
    {80, 4}, {83, 8}, {84, 13}, {82, 19}, {76, 25}, {67, 30},
    {55, 33}, {42, 34}, {29, 33}, {17, 30}, {8, 25}, {2, 19},
    {0, 13}, {1, 8}, {4, 4},
};
constexpr MouthPoint kBoredMouth[] = {
    {9, 4}, {21, 6}, {34, 7}, {48, 6}, {62, 3}, {73, 1},
    {78, 2}, {82, 6}, {84, 12}, {83, 18}, {79, 23}, {72, 26},
    {59, 28}, {45, 30}, {30, 30}, {17, 28}, {8, 25}, {3, 21},
    {0, 16}, {0, 11}, {3, 6},
};
constexpr MouthPoint kSleepyMouth[] = {
    {24, 1}, {36, 0}, {48, 1}, {59, 4}, {67, 9}, {71, 14},
    {72, 19}, {69, 25}, {62, 30}, {50, 33}, {36, 34}, {22, 33},
    {10, 30}, {3, 25}, {0, 19}, {1, 14}, {5, 9}, {13, 4},
};
constexpr MouthPoint kSurprisedMouth[] = {
    {17, 4},  {26, 2},  {34, 1},  {42, 1},  {49, 3},  {56, 6},  {61, 9},  {65, 14},
    {67, 20}, {69, 28}, {69, 35}, {67, 41}, {63, 47}, {58, 51}, {52, 54}, {45, 56},
    {36, 56}, {27, 55}, {19, 54}, {13, 50}, {7, 46},  {3, 41},  {1, 35},  {0, 29},
    {1, 22},  {3, 16},  {6, 11},  {11, 7},  {17, 4},
};
constexpr MouthPoint kAngryMouth[] = {
    {6, 14},  {14, 10}, {23, 6},  {32, 4},  {40, 2},  {44, 1},  {49, 1},  {53, 2},
    {57, 3},  {65, 5},  {72, 8},  {79, 11}, {86, 15}, {89, 17}, {91, 20}, {92, 23},
    {92, 27}, {92, 30}, {90, 31}, {88, 31}, {85, 30}, {75, 26}, {66, 23}, {57, 22},
    {48, 21}, {39, 21}, {29, 23}, {19, 26}, {8, 30},  {4, 31},  {2, 31},  {0, 30},
    {0, 27},  {0, 23},  {2, 20},  {3, 17},  {6, 14},
};

constexpr std::array<MouthGeometry, 6> kMouthGeometries = {{
    {"neutral", 82, 124, 76, 30, 15, 256, 0, 24, kNeutralMouth, std::size(kNeutralMouth)},
    {"happy", 78, 120, 84, 34, 13, 256, 0, 24, kHappyMouth, std::size(kHappyMouth)},
    {"bored", 78, 120, 84, 30, 14, 256, -12, 32, kBoredMouth, std::size(kBoredMouth)},
    {"sleepy", 84, 120, 72, 34, -2, 256, 0, 32, kSleepyMouth, std::size(kSleepyMouth)},
    // With the taller surprised eyes/mouth and extra gap, +12 centers their
    // combined resting bounds at y=120 on the 240px screen.
    {"surprised", 85, 116, 70, 56, 12, 256, -14, 64, kSurprisedMouth, std::size(kSurprisedMouth)},
    {"angry", 74, 121, 93, 32, 13, 256, -24, 72, kAngryMouth, std::size(kAngryMouth)},
}};

const MouthGeometry* FindMouthGeometry(const std::string& emotion) {
    auto found = std::find_if(kMouthGeometries.begin(), kMouthGeometries.end(),
                              [&emotion](const auto& item) { return emotion == item.emotion; });
    return found == kMouthGeometries.end() ? nullptr : &*found;
}

bool PointInMouth(float x, float y, const MouthGeometry& geometry) {
    bool inside = false;
    for (size_t i = 0, previous = geometry.point_count - 1; i < geometry.point_count;
         previous = i++) {
        const auto& a = geometry.points[i];
        const auto& b = geometry.points[previous];
        if (((a.y > y) != (b.y > y)) &&
            x < (b.x - a.x) * (y - a.y) / static_cast<float>(b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}



int TriangleWave(uint16_t phase, int period, int amplitude) {
    const int position = phase % period;
    const int half = period / 2;
    const int ramp = position < half ? position : period - position;
    return ramp * amplitude * 2 / half - amplitude;
}

}  // namespace

bool MochanDisplay::HasMouthGeometry(const std::string& emotion) {
    return FindMouthGeometry(emotion) != nullptr;
}

bool MochanDisplay::GetMouthIdleEyeOffset(const std::string& emotion, int& offset_y) {
    const auto* geometry = FindMouthGeometry(emotion);
    if (geometry == nullptr) {
        return false;
    }
    offset_y = geometry->idle_eye_offset_y;
    return true;
}

bool MochanDisplay::InitializeEyeRasters() {
    constexpr size_t bytes = EyeRaster::kWidth * EyeRaster::kHeight * sizeof(uint32_t);
    for (auto* raster : {&left_raster_, &right_raster_}) {
        // Allocate once in PSRAM, never in the animation/audio loop. If PSRAM
        // is unavailable, preserve the existing lightweight rounded-eye UI.
        raster->pixels =
            static_cast<uint32_t*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (raster->pixels == nullptr) {
            for (auto* allocated : {&left_raster_, &right_raster_}) {
                heap_caps_free(allocated->pixels);
                allocated->pixels = nullptr;
            }
            ESP_LOGW(kTag, "Eye raster allocation unavailable; using rounded eyes");
            return false;
        }
        raster->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        raster->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
        raster->descriptor.header.w = EyeRaster::kWidth;
        raster->descriptor.header.h = EyeRaster::kHeight;
        raster->descriptor.header.stride = EyeRaster::kWidth * sizeof(uint32_t);
        raster->descriptor.data_size = bytes;
        raster->descriptor.data = reinterpret_cast<const uint8_t*>(raster->pixels);
    }
    return true;
}

bool MochanDisplay::InitializeMouthRaster() {
    constexpr size_t bytes = MouthRaster::kWidth * MouthRaster::kHeight * sizeof(uint32_t);
    mouth_raster_.pixels =
        static_cast<uint32_t*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (mouth_raster_.pixels == nullptr) {
        heap_caps_free(mouth_raster_.pixels);
        mouth_raster_.pixels = nullptr;
        ESP_LOGW(kTag, "Mouth raster allocation unavailable; keeping the legacy eye-only face");
        return false;
    }
    mouth_raster_.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    mouth_raster_.descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
    mouth_raster_.descriptor.header.w = MouthRaster::kWidth;
    mouth_raster_.descriptor.header.h = MouthRaster::kHeight;
    mouth_raster_.descriptor.header.stride = MouthRaster::kWidth * sizeof(uint32_t);
    mouth_raster_.descriptor.data_size = bytes;
    mouth_raster_.descriptor.data = reinterpret_cast<const uint8_t*>(mouth_raster_.pixels);
    return true;
}

bool MochanDisplay::RenderMouthTarget(const std::string& emotion) {
    const auto* geometry = FindMouthGeometry(emotion);
    if (geometry == nullptr || mouth_raster_.pixels == nullptr) {
        return false;
    }
    if (mouth_raster_.rendered_emotion == emotion) {
        return false;
    }
    const int64_t render_started_us = esp_timer_get_time();
    mouth_raster_.rendered_emotion = emotion;

    constexpr int kRasterScreenX = (240 - MouthRaster::kWidth) / 2;
    constexpr int kRasterScreenY = (240 - MouthRaster::kHeight) / 2 + 25;
    constexpr std::array<float, 2> kSamples = {0.25f, 0.75f};
    const int local_x = geometry->x - kRasterScreenX;
    const int local_y = geometry->y - kRasterScreenY;

    std::fill_n(mouth_raster_.pixels, MouthRaster::kWidth * MouthRaster::kHeight, 0);
    for (int y = std::max(0, local_y - 10); y < std::min(MouthRaster::kHeight, local_y + 72); ++y) {
        for (int x = std::max(0, local_x - 2); x < std::min(MouthRaster::kWidth, local_x + 96);
             ++x) {
            int dark_samples = 0;
            int bright_samples = 0;
            for (float sample_y : kSamples) {
                for (float sample_x : kSamples) {
                    const float shape_x = x + sample_x - local_x;
                    const float shape_y = y + sample_y - local_y;
                    if (!PointInMouth(shape_x, shape_y, *geometry)) {
                        continue;
                    }
                    ++dark_samples;
                    // The approved mock clips a brighter copy of the same solid shape,
                    // shifted four pixels right and up, matching the eye's layered brass.
                    if (PointInMouth(shape_x - 4, shape_y + 4, *geometry)) {
                        ++bright_samples;
                    }
                }
            }
            if (dark_samples == 0) {
                continue;
            }
            const uint32_t color = bright_samples * 2 >= dark_samples ? 0xc6a15b : 0x896a36;
            const uint32_t alpha = static_cast<uint32_t>(dark_samples * 255 / 4);
            mouth_raster_.pixels[y * MouthRaster::kWidth + x] = (alpha << 24) | color;
        }
    }
    lv_image_cache_drop(&mouth_raster_.descriptor);
    ESP_LOGD(kTag, "Mouth raster %s: %lld us", emotion.c_str(),
             static_cast<long long>(esp_timer_get_time() - render_started_us));
    return true;
}

void MochanDisplay::UpdateMouth(uint8_t blink_amount, const std::string& current_emotion) {
    if (mouth_ == nullptr || mouth_raster_.pixels == nullptr) {
        return;
    }
    std::string emotion = current_emotion;
    if (face_layout_target_ == 0 && !exiting_mouth_emotion_.empty()) {
        emotion = exiting_mouth_emotion_;
    }
    if (FindMouthGeometry(emotion) == nullptr || face_layout_progress_ == 0) {
        if (!lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        }
        if (face_layout_progress_ == 0) {
            exiting_mouth_emotion_.clear();
            mouth_shape_opacity_ = 0;
        }
        return;
    }

    const auto* geometry = FindMouthGeometry(emotion);
    // Fade out the old silhouette before swapping it. Eyes retain their
    // approved interpolation, and the mouth never snaps between expressions.
    if (mouth_raster_.rendered_emotion != emotion && mouth_shape_opacity_ > 0) {
        mouth_shape_opacity_ = std::max(0, mouth_shape_opacity_ - 48);
        geometry = FindMouthGeometry(mouth_raster_.rendered_emotion);
        if (geometry != nullptr && mouth_shape_opacity_ > 0) {
            emotion = geometry->emotion;
        } else {
            geometry = FindMouthGeometry(emotion);
        }
    } else {
        mouth_shape_opacity_ = std::min(256, mouth_shape_opacity_ + 48);
    }
    if (RenderMouthTarget(emotion)) {
        lv_obj_invalidate(mouth_);
    }
    constexpr int kRasterScreenY = (240 - MouthRaster::kHeight) / 2 + 25;
    const int pivot_y = geometry->y - kRasterScreenY + geometry->height / 2;
    if (mouth_raster_.previous_pivot_y != pivot_y) {
        mouth_raster_.previous_pivot_y = pivot_y;
        lv_obj_set_style_transform_pivot_y(mouth_, pivot_y, 0);
    }
    // Fade layout transitions without shrinking the mouth relative to eyes.
    int expression_scale_x = 256;
    int expression_deformation_y = 0;
    if (emotion == "surprised") {
        const int pulse = TriangleWave(animation_phase_, 30, 2);
        expression_scale_x += pulse * 2;
        expression_deformation_y += pulse * 5;
    } else if (emotion == "angry") {
        const int tension = TriangleWave(animation_phase_, 40, 2);
        expression_scale_x -= tension * 3;
        expression_deformation_y += tension * 3;
    }
    const int scale_x =
        (256 - yawn_amount_ * 48 / 256) * expression_scale_x / 256;
    const int deformation_y = geometry->base_scale_y + expression_deformation_y +
                              yawn_amount_ * (emotion == "sleepy" ? 112 : 160) / 256 +
                              mouth_motion_amount_ * geometry->idle_open_scale_y / 256 +
                              blink_amount * geometry->blink_scale_y / 100;
    // Blinks and the closed phase may soften the expression, never collapse
    // a filled mouth into a thin stroke. Preserve at least 94% of its height.
    const int scale_y = std::max(240, deformation_y);
    if (mouth_raster_.previous_scale_x != scale_x) {
        mouth_raster_.previous_scale_x = scale_x;
        lv_image_set_scale_x(mouth_, scale_x);
    }
    if (mouth_raster_.previous_scale_y != scale_y) {
        mouth_raster_.previous_scale_y = scale_y;
        lv_image_set_scale_y(mouth_, scale_y);
    }
    const uint8_t opacity = face_layout_progress_ * mouth_shape_opacity_ / 256 * LV_OPA_COVER / 256;
    if (mouth_raster_.previous_opacity != opacity) {
        mouth_raster_.previous_opacity = opacity;
        lv_obj_set_style_opa(mouth_, opacity, 0);
    }
    // Follow the actual smoothed eye center, including its gentle vertical
    // motion. Deformation stays around the mouth's center without extra bobbing.
    const int mouth_x = (left_eye_geometry_.x + right_eye_geometry_.x) / 2;
    const int eye_center_y = (left_eye_geometry_.y + right_eye_geometry_.y) / 2;
    const int mouth_gap = kMouthEyeCenterGap +
                          (emotion == "surprised" ? kSurprisedMouthExtraGap : 0);
    const int mouth_y = 25 + 120 + eye_center_y + face_layout_offset_y_ +
                        mouth_gap - (geometry->y + geometry->height / 2);
    if (mouth_raster_.previous_x != mouth_x || mouth_raster_.previous_y != mouth_y) {
        mouth_raster_.previous_x = mouth_x;
        mouth_raster_.previous_y = mouth_y;
        lv_obj_align(mouth_, LV_ALIGN_CENTER, mouth_x, mouth_y);
    }
    if (lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool MochanDisplay::RenderEyeRaster(EyeRaster& raster, const EyeGeometry& geometry,
                                    uint8_t blink_amount) {
    const auto& previous = raster.previous;
    if (raster.rendered && raster.previous_blink == blink_amount &&
        previous.width == geometry.width && previous.height == geometry.height &&
        previous.top_curve == geometry.top_curve &&
        previous.bottom_curve == geometry.bottom_curve && previous.slope == geometry.slope &&
        previous.water == geometry.water) {
        return false;
    }
    raster.previous = geometry;
    raster.previous_blink = blink_amount;
    raster.rendered = true;
    const float openness = 1.0f - blink_amount / 100.0f;
    const float height = std::max(7.0f, geometry.height * openness);
    const float half_width = geometry.width * 0.5f;
    const auto rounded_distance = [](float x, float y, float half_w, float half_h, float radius) {
        const float qx = std::fabs(x) - half_w + radius;
        const float qy = std::fabs(y) - half_h + radius;
        const float dx = std::max(qx, 0.0f);
        const float dy = std::max(qy, 0.0f);
        return (dx > 0.0f && dy > 0.0f ? std::sqrt(dx * dx + dy * dy) : dx + dy) +
               std::min(std::max(qx, qy), 0.0f) - radius;
    };
    const auto blend = [](uint32_t a, uint32_t b, float amount) {
        uint32_t result = 0;
        for (int shift : {0, 8, 16}) {
            const float first = (a >> shift) & 0xff;
            const float second = (b >> shift) & 0xff;
            result |= static_cast<uint32_t>(first + (second - first) * amount) << shift;
        }
        return result;
    };
    lv_image_cache_drop(&raster.descriptor);
    for (int x = 0; x < EyeRaster::kWidth; ++x) {
        const float px = x + 0.5f - EyeRaster::kWidth * 0.5f;
        const float u = std::clamp(px / half_width, -1.0f, 1.0f);
        const float curve = 1.0f - u * u;
        const float tilt = geometry.slope * u * openness;
        const float top = -height * 0.5f + geometry.top_curve * curve * openness + tilt;
        const float bottom = height * 0.5f + geometry.bottom_curve * curve * openness;
        const float half_height = std::max(3.5f, (bottom - top) * 0.5f);
        const float center = (top + bottom) * 0.5f;
        const float radius = std::min(18.0f, std::min(half_width, half_height));
        // A second, offset copy of the same filled shape exposes a soft left/
        // lower layer. Scale the offset down when squinting or blinking.
        const float layer_scale = std::clamp(height / 54.0f, 0.0f, 1.0f);
        const float inner_x = px - 5.0f * layer_scale;
        const float inner_u = std::clamp(inner_x / half_width, -1.0f, 1.0f);
        const float inner_curve = 1.0f - inner_u * inner_u;
        const float inner_top =
            -height * 0.5f +
            (geometry.top_curve * inner_curve + geometry.slope * inner_u) * openness;
        const float inner_bottom = height * 0.5f + geometry.bottom_curve * inner_curve * openness;
        const float inner_half_height = std::max(3.5f, (inner_bottom - inner_top) * 0.5f);
        const float inner_center = (inner_top + inner_bottom) * 0.5f - 6.0f * layer_scale;
        const float inner_radius = std::min(18.0f, std::min(half_width, inner_half_height));
        for (int y = 0; y < EyeRaster::kHeight; ++y) {
            const float py = y + 0.5f - EyeRaster::kHeight * 0.5f;
            const float distance =
                rounded_distance(px, py - center, half_width, half_height, radius);
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            if (coverage == 0.0f) {
                // Transparent pixels need no inner-layer shading or color
                // blending. Keep the exact visible geometry and antialiasing.
                raster.pixels[y * EyeRaster::kWidth + x] = 0;
                continue;
            }
            const float inner_distance = rounded_distance(inner_x, py - inner_center, half_width,
                                                          inner_half_height, inner_radius);
            const float t = std::clamp((1.0f - inner_distance) / 2.0f, 0.0f, 1.0f);
            uint32_t color = blend(0x896a36, 0xc6a15b, t * t * (3.0f - 2.0f * t));
            if (geometry.water > 0) {
                const float outer = geometry.x < 0 ? -u : u;
                const float spread = std::clamp((outer + 0.35f) / 1.35f, 0.0f, 1.0f);
                const float waterline =
                    bottom - geometry.water * openness * spread * spread * (3.0f - 2.0f * spread);
                const float water_mix = std::clamp((py - waterline + 1.0f) / 2.0f, 0.0f, 1.0f);
                color = blend(color, 0x80643b, water_mix);
            }
            raster.pixels[y * EyeRaster::kWidth + x] =
                (static_cast<uint32_t>(coverage * 255.0f) << 24) | color;
        }
    }
    return true;
}

void MochanDisplay::ApplyRoundedEye(lv_obj_t* eye, lv_obj_t* shadow, const EyeGeometry& geometry,
                                    uint8_t blink_amount) {
    auto& raster = eye == left_eye_ ? left_raster_ : right_raster_;
    if (raster.pixels != nullptr) {
        if (!lv_obj_has_flag(shadow, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(shadow, LV_OBJ_FLAG_HIDDEN);
        }
        const bool pixels_changed = RenderEyeRaster(raster, geometry, blink_amount);
        if (!raster.positioned) {
            lv_obj_set_style_transform_pivot_x(eye, EyeRaster::kWidth / 2, 0);
            lv_obj_set_style_transform_pivot_y(eye, EyeRaster::kHeight / 2, 0);
        }
        if (!raster.positioned || raster.displayed_rotation != geometry.rotation) {
            raster.displayed_rotation = geometry.rotation;
            lv_obj_set_style_transform_rotation(eye, geometry.rotation, 0);
        }
        if (!raster.positioned || raster.displayed_x != geometry.x ||
            raster.displayed_y != geometry.y) {
            raster.displayed_x = geometry.x;
            raster.displayed_y = geometry.y;
            lv_obj_align(eye, LV_ALIGN_CENTER, geometry.x, geometry.y);
        }
        raster.positioned = true;
        if (pixels_changed) {
            lv_obj_invalidate(eye);
        }
        return;
    }
    if (raster.rendered && raster.previous_blink == blink_amount &&
        raster.previous.width == geometry.width && raster.previous.height == geometry.height &&
        raster.previous.x == geometry.x && raster.previous.y == geometry.y &&
        raster.previous.rotation == geometry.rotation &&
        raster.previous.top_curve == geometry.top_curve &&
        raster.previous.bottom_curve == geometry.bottom_curve &&
        raster.previous.slope == geometry.slope && raster.previous.water == geometry.water) {
        return;
    }
    raster.previous = geometry;
    raster.previous_blink = blink_amount;
    raster.rendered = true;
    const int height = std::max(7, geometry.height - (geometry.height - 7) * blink_amount / 100);
    const int inset_x = std::min(4, std::max(2, geometry.width / 8));
    const int inset_y = std::min(6, std::max(2, height / 4));
    const int shadow_radius = std::clamp(std::min(geometry.width, height) / 3, 6, 18);

    lv_obj_remove_flag(shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(shadow, geometry.width, height);
    lv_obj_set_style_radius(shadow, shadow_radius, 0);
    lv_obj_set_style_transform_pivot_x(shadow, geometry.width / 2, 0);
    lv_obj_set_style_transform_pivot_y(shadow, height / 2, 0);
    lv_obj_set_style_transform_rotation(shadow, geometry.rotation, 0);
    lv_obj_align(shadow, LV_ALIGN_CENTER, geometry.x, geometry.y);

    const int bright_width = std::max(8, geometry.width - inset_x);
    const int bright_height = std::max(4, height - inset_y);
    lv_obj_set_size(eye, bright_width, bright_height);
    lv_obj_set_style_radius(eye, std::max(5, shadow_radius - 1), 0);
    lv_obj_set_style_transform_pivot_x(eye, bright_width / 2, 0);
    lv_obj_set_style_transform_pivot_y(eye, bright_height / 2, 0);
    lv_obj_set_style_transform_rotation(eye, geometry.rotation, 0);
    lv_obj_align(eye, LV_ALIGN_CENTER, geometry.x - inset_x / 2, geometry.y - inset_y / 2);
}

void MochanDisplay::UpdateEyes(uint8_t blink_amount, bool idle_eligible) {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    struct EyeTarget {
        EyeGeometry geometry;
    };
    EyeTarget left{{74, 54, -48, -59, 0}};
    EyeTarget right{{74, 54, 48, -59, 0}};

    const int gentle = TriangleWave(animation_phase_, 32, 2);

    switch (face_state_) {
        case FaceState::kListening:
            left.geometry = {78, 54 + gentle, -48, -59, 0};
            right.geometry = {78, 54 + gentle, 48, -59, 0};
            break;
        case FaceState::kSpeaking: {
            const int voice = TriangleWave(animation_phase_, 18, 5);
            left.geometry = {70, 43 + voice, -48, -59, 0};
            right.geometry = {70, 43 - voice, 48, -59, 0};
            break;
        }
        case FaceState::kThinking:
            left.geometry = {60, 54, -53, -63, 0, 3, 0, -5};
            right.geometry = {68, 34, 45, -53, 0, 8, 0, 3};
            break;
        case FaceState::kHappy:
            left.geometry = {74, 43, -47, -56 + gentle, 0, -5, -17};
            right.geometry = {74, 43, 47, -56 + gentle, 0, -5, -17};
            break;
        case FaceState::kLaughing: {
            const int bounce = TriangleWave(animation_phase_, 16, 4);
            left.geometry = {76, 38, -47, -54 + bounce, 0, -9, -19, -2};
            right.geometry = {76, 38, 47, -54 + bounce, 0, -9, -19, 2};
            break;
        }
        case FaceState::kFunny: {
            const int sway = TriangleWave(animation_phase_, 30, 4);
            left.geometry = {54, 61, -48 + sway, -61, -60, 0, -3};
            right.geometry = {72, 40, 48 + sway, -54, 40, -5, -18};
            break;
        }
        case FaceState::kAngry: {
            const int tension = TriangleWave(animation_phase_, 40, 2);
            left.geometry = {74, 43, -45 + tension, -55 + tension / 2, 0, 2, 0, 12};
            right.geometry = {74, 43, 45 - tension, -55 + tension / 2, 0, 2, 0, -12};
            break;
        }
        case FaceState::kSad:
            left.geometry = {72, 49, -47, -55, 0, 10, 0, -9};
            right.geometry = {72, 49, 47, -55, 0, 10, 0, 9};
            break;
        case FaceState::kCrying:
            left.geometry = {72, 49, -47, -55 + gentle, 0, 10, 0, -9, 18};
            right.geometry = {72, 49, 47, -55 + gentle, 0, 10, 0, 9, 18};
            break;
        case FaceState::kLoving: {
            const int pulse = TriangleWave(animation_phase_, 36, 2);
            left.geometry = {62 + pulse, 48 + pulse, -40, -57, 60, -4, -12, 2};
            right.geometry = {62 + pulse, 48 + pulse, 40, -57, -60, -4, -12, -2};
            break;
        }
        case FaceState::kEmbarrassed:
            left.geometry = {59, 38, -51, -47 + gentle, 0, 8, -3, -5};
            right.geometry = {59, 38, 41, -47 + gentle, 0, 8, -3, 5};
            break;
        case FaceState::kSurprised: {
            const int pulse = TriangleWave(animation_phase_, 30, 2);
            const int spread = pulse / 2;
            left.geometry = {49, 65, -44 - spread, -57, 0};
            right.geometry = {49, 65, 44 + spread, -57, 0};
            break;
        }
        case FaceState::kShocked: {
            const int tremble = TriangleWave(animation_phase_, 10, 2);
            left.geometry = {59, 69, -44 + tremble, -57, 0};
            right.geometry = {49, 72, 44 + tremble, -59, 0};
            break;
        }
        case FaceState::kWinking:
            left.geometry = {69, 30, -47, -53, 0, -6, -15};
            right.geometry = {70, 54, 47, -58, 0, 0, -5};
            break;
        case FaceState::kCool:
            left.geometry = {78, 33, -46, -55, 0, 0, 0, -3};
            right.geometry = {78, 33, 46, -55, 0, 0, 0, 3};
            break;
        case FaceState::kRelaxed:
            left.geometry = {71, 40, -47, -54 + gentle, 0, 4, -6};
            right.geometry = {71, 40, 47, -54 + gentle, 0, 4, -6};
            break;
        case FaceState::kDelicious: {
            const int savor = TriangleWave(animation_phase_, 32, 3);
            left.geometry = {69, 41, -45, -54 + savor, 0, -4, -15};
            right.geometry = {69, 41, 45, -54 - savor, 0, -4, -15};
            break;
        }
        case FaceState::kKissy:
            left.geometry = {53, 33, -37, -54, -80, -3, -12};
            right.geometry = {53, 33, 37, -54, 80, -3, -12};
            break;
        case FaceState::kConfident:
            left.geometry = {70, 51, -46, -61, 0, -3, 0, 3};
            right.geometry = {73, 34, 46, -53, 0, 4, -2, -4};
            break;
        case FaceState::kSleepy:
            left.geometry = {71, 28, -46, -48 + gentle, 0, 7, 0};
            right.geometry = {71, 28, 46, -48 + gentle, 0, 7, 0};
            break;
        case FaceState::kSilly: {
            const int sway = TriangleWave(animation_phase_, 24, 4);
            left.geometry = {48, 62, -48 + sway, -64, 80, 0, 0, -3};
            right.geometry = {77, 33, 48 + sway, -46, -80, 2, -8};
            break;
        }
        case FaceState::kConfused:
            left.geometry = {69, 33, -49, -49, 0, 8, 0, -7};
            right.geometry = {54, 57, 48, -63, 0, -3, 0, 3};
            break;
        case FaceState::kSuspicious:
            left.geometry = {74, 31, -40, -52, 0, 6, 0, 3};
            right.geometry = {63, 43, 54, -59, 0, 9, 0, -3};
            break;
        case FaceState::kShake: {
            const int shake = TriangleWave(animation_phase_, 12, 11);
            left.geometry = {72, 46, -48 + shake, -57, 0};
            right.geometry = {72, 46, 48 + shake, -57, 0};
            break;
        }
        case FaceState::kLookLeft:
            left.geometry.x -= 18;
            right.geometry.x -= 18;
            break;
        case FaceState::kLookRight:
            left.geometry.x += 18;
            right.geometry.x += 18;
            break;
        case FaceState::kLookUp:
            left.geometry.y = -72;
            right.geometry.y = -72;
            break;
        case FaceState::kLookDown:
            left.geometry.y = -34;
            right.geometry.y = -34;
            break;
        case FaceState::kLookUpLeft:
            left.geometry.x -= 14;
            right.geometry.x -= 14;
            left.geometry.y = -70;
            right.geometry.y = -70;
            break;
        case FaceState::kLookUpRight:
            left.geometry.x += 14;
            right.geometry.x += 14;
            left.geometry.y = -70;
            right.geometry.y = -70;
            break;
        case FaceState::kLookDownLeft:
            left.geometry.x -= 14;
            right.geometry.x -= 14;
            left.geometry.y = -35;
            right.geometry.y = -35;
            break;
        case FaceState::kLookDownRight:
            left.geometry.x += 14;
            right.geometry.x += 14;
            left.geometry.y = -35;
            right.geometry.y = -35;
            break;
        case FaceState::kIdle:
            break;
    }

    int target_gaze_x = 0;
    int target_gaze_y = 0;
    if (idle_eligible) {
        int gaze_amplitude = 8;
        int gaze_down = 4;
        if (face_state_ == FaceState::kHappy) {
            gaze_amplitude = 6;
            gaze_down = 3;
        } else if (face_state_ == FaceState::kCool) {
            gaze_amplitude = 7;
            gaze_down = 3;
        } else if (face_state_ == FaceState::kSleepy) {
            gaze_amplitude = 4;
            gaze_down = 2;
        } else if (face_state_ == FaceState::kSurprised) {
            gaze_amplitude = 5;
            gaze_down = 1;
        }
        const int idle_phase = idle_motion_phase_ % 240;
        if (idle_phase >= 45 && idle_phase < 85) {
            target_gaze_x = -gaze_amplitude;
            target_gaze_y = std::max(1, gaze_down - 1);
        } else if (idle_phase >= 145 && idle_phase < 185) {
            target_gaze_x = gaze_amplitude;
            target_gaze_y = gaze_down;
        }
    }
    idle_gaze_x_ += std::clamp(target_gaze_x - idle_gaze_x_, -1, 1);
    idle_gaze_y_ += std::clamp(target_gaze_y - idle_gaze_y_, -1, 1);
    left.geometry.x += idle_gaze_x_;
    right.geometry.x += idle_gaze_x_;
    left.geometry.y += idle_gaze_y_;
    right.geometry.y += idle_gaze_y_;

    if (idle_eligible) {
        const int positive_mouth_motion = std::max<int>(0, mouth_motion_amount_);
        const int mouth_reaction = positive_mouth_motion * 2 / 256;
        if (face_state_ == FaceState::kSurprised) {
            left.geometry.height += mouth_reaction;
            right.geometry.height += mouth_reaction;
        } else {
            left.geometry.height = std::max(7, left.geometry.height - mouth_reaction);
            right.geometry.height = std::max(7, right.geometry.height - mouth_reaction);
            left.geometry.y -= positive_mouth_motion / 256;
            right.geometry.y -= positive_mouth_motion / 256;
        }
    }

    left.geometry.y += kEyeLayoutOffsetY;
    right.geometry.y += kEyeLayoutOffsetY;

    // Keep the existing smoothing for intrinsic eye/emotion geometry. The
    // layout offset is applied after this block so it follows elapsed time
    // directly and does not acquire a second transition tail.
    const auto smooth = [](int current, int target) {
        const int delta = target - current;
        if (delta >= -1 && delta <= 1) {
            return target;
        }
        const int step = delta / 3;
        return current + (step != 0 ? step : (delta > 0 ? 1 : -1));
    };
    const auto approach = [&smooth](EyeGeometry& current, const EyeGeometry& target) {
        current.width = smooth(current.width, target.width);
        current.height = smooth(current.height, target.height);
        current.x = smooth(current.x, target.x);
        current.y = smooth(current.y, target.y);
        current.rotation = smooth(current.rotation, target.rotation);
        current.top_curve = smooth(current.top_curve, target.top_curve);
        current.bottom_curve = smooth(current.bottom_curve, target.bottom_curve);
        current.slope = smooth(current.slope, target.slope);
        current.water = smooth(current.water, target.water);
    };
    if (!eye_geometry_initialized_) {
        left_eye_geometry_ = left.geometry;
        right_eye_geometry_ = right.geometry;
        eye_geometry_initialized_ = true;
    } else {
        approach(left_eye_geometry_, left.geometry);
        approach(right_eye_geometry_, right.geometry);
    }

    EyeGeometry displayed_left = left_eye_geometry_;
    EyeGeometry displayed_right = right_eye_geometry_;
    displayed_left.y += face_layout_offset_y_;
    displayed_right.y += face_layout_offset_y_;
    ApplyRoundedEye(left_eye_, left_eyelid_, displayed_left, blink_amount);
    ApplyRoundedEye(right_eye_, right_eyelid_, displayed_right, blink_amount);
}
// End of Mochan face rendering implementation.
