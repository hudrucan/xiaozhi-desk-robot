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
constexpr char kTag[] = "MochanDisplay";

struct MouthGeometry {
    const char* emotion;
    int width;
    int height;
    float top_curve;
    float bottom_curve;
    float slope;
    int gap;
    int idle_eye_offset_y;
    int base_scale_y;
    int blink_scale_y;
    int idle_open_scale_y;
};

constexpr std::array<MouthGeometry, 24> kMouthGeometries = {{
    // Base expressions
    {"neutral", 72, 24, 2.0f, 2.0f, 0.0f, 74, 20, 256, -88, 24},
    {"happy", 80, 20, 2.5f, 12.0f, 0.0f, 64, 25, 256, -80, 24},
    {"bored", 76, 22, 0.0f, 0.0f, -1.5f, 72, 16, 256, -88, 32},
    {"sleepy", 56, 28, -6.0f, 6.0f, 0.0f, 70, 14, 256, -88, 32},
    {"surprised", 54, 38, -3.0f, 10.0f, 0.0f, 78, 17, 256, -96, 64},
    {"angry", 78, 24, -10.0f, -6.0f, 0.0f, 74, 17, 256, -96, 72},
    {"sad", 68, 20, -6.0f, -4.0f, 0.0f, 66, 23, 256, -80, 24},
    {"crying", 62, 28, -7.5f, 6.0f, 0.0f, 68, 20, 256, -88, 32},

    // Extended emotional expressions
    {"laughing", 84, 36, 4.0f, 20.0f, 0.0f, 62, 19, 256, -96, 64},
    {"loving", 54, 20, 2.0f, 9.0f, 0.0f, 62, 25, 256, -80, 24},
    {"winking", 74, 22, 1.5f, 8.0f, 3.5f, 64, 22, 256, -80, 24},
    {"kissy", 36, 28, -2.0f, 3.0f, 0.0f, 62, 19, 256, -70, 16},
    {"shocked", 52, 48, -4.0f, 12.0f, 0.0f, 76, 17, 256, -96, 72},
    {"embarrassed", 56, 16, -2.0f, 1.0f, 2.0f, 64, 22, 256, -80, 24},
    {"delicious", 74, 26, 2.5f, 14.0f, 0.0f, 64, 21, 256, -80, 32},
    {"confident", 72, 22, 1.0f, 6.0f, 3.0f, 66, 21, 256, -80, 24},
    {"cool", 66, 16, 0.0f, 2.0f, 2.0f, 68, 19, 256, -80, 24},
    {"relaxed", 68, 18, 1.5f, 5.0f, 0.0f, 68, 20, 256, -80, 24},
    {"confused", 58, 20, -2.0f, 2.0f, 3.5f, 66, 22, 256, -80, 24},
    {"suspicious", 64, 16, -1.0f, -1.0f, -1.5f, 68, 20, 256, -80, 24},

    // Additional expressions matching control panel
    {"funny", 68, 26, -3.0f, 6.0f, -4.0f, 68, 21, 256, -80, 24},
    {"silly", 70, 28, -2.0f, 12.0f, -4.0f, 66, 21, 256, -80, 32},
    {"thinking", 52, 18, 1.0f, 0.0f, 3.0f, 66, 22, 256, -70, 16},
    {"shake", 66, 22, 0.0f, 4.0f, 0.0f, 70, 20, 256, -80, 24},
}};

const MouthGeometry* FindMouthGeometry(const std::string& emotion) {
    auto found = std::find_if(kMouthGeometries.begin(), kMouthGeometries.end(),
                              [&emotion](const auto& item) { return emotion == item.emotion; });
    return found == kMouthGeometries.end() ? nullptr : &*found;
}

int TriangleWave(uint16_t phase, int period, int amplitude) {
    const int position = phase % period;
    const int half = period / 2;
    const int ramp = position < half ? position : period - position;
    return ramp * amplitude * 2 / half - amplitude;
}

float DistToSegment(float px, float py, float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len_sq = dx * dx + dy * dy;
    const float u = len_sq > 0.0f ? std::clamp(((px - x1) * dx + (py - y1) * dy) / len_sq, 0.0f, 1.0f) : 0.0f;
    const float qx = px - (x1 + u * dx);
    const float qy = py - (y1 + u * dy);
    return std::sqrt(qx * qx + qy * qy);
}

float DistToZ(float px, float py, float cx, float cy, float w, float h) {
    const float lx = px - cx;
    const float ly = py - cy;
    const float x0 = -w * 0.5f;
    const float x1 =  w * 0.5f;
    const float y0 = -h * 0.5f;
    const float y1 =  h * 0.5f;
    const float d1 = DistToSegment(lx, ly, x0, y0, x1, y0);
    const float d2 = DistToSegment(lx, ly, x1, y0, x0, y1);
    const float d3 = DistToSegment(lx, ly, x0, y1, x1, y1);
    return std::min({d1, d2, d3});
}

float RoundedDistance(float x, float y, float half_width, float half_height, float radius) {
    const float qx = std::fabs(x) - half_width + radius;
    const float qy = std::fabs(y) - half_height + radius;
    const float dx = std::max(qx, 0.0f);
    const float dy = std::max(qy, 0.0f);
    return (dx > 0.0f && dy > 0.0f ? std::sqrt(dx * dx + dy * dy) : dx + dy) +
           std::min(std::max(qx, qy), 0.0f) - radius;
}

uint32_t BlendRgb(uint32_t first_color, uint32_t second_color, float amount) {
    uint32_t result = 0;
    for (int shift : {0, 8, 16}) {
        const float first = (first_color >> shift) & 0xff;
        const float second = (second_color >> shift) & 0xff;
        result |= static_cast<uint32_t>(first + (second - first) * amount) << shift;
    }
    return result;
}

float SmoothStep(float value) {
    return value * value * (3.0f - 2.0f * value);
}

void RenderMouthSdf(uint32_t* pixels, lv_image_dsc_t* descriptor, int raster_width,
                    int raster_height, float width, float height, float top_curve,
                    float bottom_curve, float slope) {
    if (pixels == nullptr || descriptor == nullptr) {
        return;
    }
    const float h = std::max(7.0f, height);
    const float half_width = width * 0.5f;
    lv_image_cache_drop(descriptor);
    for (int x = 0; x < raster_width; ++x) {
        const float px = x + 0.5f - raster_width * 0.5f;
        const float u = std::clamp(px / half_width, -1.0f, 1.0f);
        const float curve = 1.0f - u * u;
        const float tilt = slope * u;
        const float top = -h * 0.5f + top_curve * curve + tilt;
        const float bottom = h * 0.5f + bottom_curve * curve;
        const float half_height = std::max(3.5f, (bottom - top) * 0.5f);
        const float center = (top + bottom) * 0.5f;
        const float radius = std::min(18.0f, std::min(half_width, half_height));

        const float layer_scale = std::clamp(h / 54.0f, 0.0f, 1.0f);
        const float inner_x = px - 5.0f * layer_scale;
        const float inner_u = std::clamp(inner_x / half_width, -1.0f, 1.0f);
        const float inner_curve = 1.0f - inner_u * inner_u;
        const float inner_top = -h * 0.5f + (top_curve * inner_curve + slope * inner_u);
        const float inner_bottom = h * 0.5f + bottom_curve * inner_curve;
        const float inner_half_height = std::max(3.5f, (inner_bottom - inner_top) * 0.5f);
        const float inner_center = (inner_top + inner_bottom) * 0.5f - 6.0f * layer_scale;
        const float inner_radius = std::min(18.0f, std::min(half_width, inner_half_height));

        for (int y = 0; y < raster_height; ++y) {
            const float py = y + 0.5f - raster_height * 0.5f;
            const float distance = RoundedDistance(px, py - center, half_width, half_height, radius);
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            if (coverage == 0.0f) {
                pixels[y * raster_width + x] = 0;
                continue;
            }
            const float inner_distance = RoundedDistance(inner_x, py - inner_center, half_width,
                                                          inner_half_height, inner_radius);
            const float t = std::clamp((1.0f - inner_distance) / 2.0f, 0.0f, 1.0f);
            const uint32_t color = BlendRgb(0x896a36, 0xc6a15b, SmoothStep(t));
            pixels[y * raster_width + x] =
                (static_cast<uint32_t>(coverage * 255.0f) << 24) | color;
        }
    }
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
    mouth_morph_state_ = {};
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

bool MochanDisplay::InitializeSleepyZzRaster() {
    sleepy_zz_raster_ = {};
    constexpr size_t bytes =
        SleepyZzRaster::kWidth * SleepyZzRaster::kHeight * sizeof(uint32_t);
    sleepy_zz_raster_.pixels =
        static_cast<uint32_t*>(
            heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (sleepy_zz_raster_.pixels == nullptr) {
        ESP_LOGW(kTag, "Sleepy zZ raster allocation unavailable");
        return false;
    }

    sleepy_zz_raster_.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    sleepy_zz_raster_.descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
    sleepy_zz_raster_.descriptor.header.w = SleepyZzRaster::kWidth;
    sleepy_zz_raster_.descriptor.header.h = SleepyZzRaster::kHeight;
    sleepy_zz_raster_.descriptor.header.stride =
        SleepyZzRaster::kWidth * sizeof(uint32_t);
    sleepy_zz_raster_.descriptor.data_size = bytes;
    sleepy_zz_raster_.descriptor.data =
        reinterpret_cast<const uint8_t*>(sleepy_zz_raster_.pixels);

    // Preserve the original glyph coordinates relative to the right eye while
    // storing them in an independent 40x40 overlay centered at (+24, -32).
    for (int x = 0; x < SleepyZzRaster::kWidth; ++x) {
        const float px = x + 0.5f - SleepyZzRaster::kWidth * 0.5f + 24.0f;
        for (int y = 0; y < SleepyZzRaster::kHeight; ++y) {
            const float py = y + 0.5f - SleepyZzRaster::kHeight * 0.5f - 32.0f;
            if (px < 10.0f || px > 38.0f || py < -46.0f || py > -18.0f) {
                continue;
            }

            const float d_big = DistToZ(px, py, 26.0f, -34.0f, 13.0f, 15.0f) - 2.1f;
            const float d_small_raw = DistToZ(px, py, 19.5f, -26.0f, 9.0f, 10.0f);
            const float d_small = d_small_raw - 1.6f;
            const float d_small_outline = d_small_raw - 3.2f;
            const float big_coverage = std::clamp(0.5f - d_big, 0.0f, 1.0f);
            uint32_t color = 0;
            float alpha = 0.0f;

            if (big_coverage > 0.0f) {
                const float inner_distance =
                    DistToZ(px - 1.0f, py + 1.2f, 26.0f, -34.0f, 13.0f, 15.0f) -
                    2.1f;
                const float blend =
                    std::clamp((0.5f - inner_distance) / 1.5f, 0.0f, 1.0f);
                color = BlendRgb(0x896a36, 0xc6a15b, SmoothStep(blend));
                alpha = big_coverage;
            }

            const float outline_coverage =
                std::clamp(0.5f - d_small_outline, 0.0f, 1.0f);
            if (outline_coverage > 0.0f) {
                color = BlendRgb(color, 0x000000, outline_coverage);
                alpha = std::max(alpha, outline_coverage);
            }

            const float small_coverage = std::clamp(0.5f - d_small, 0.0f, 1.0f);
            if (small_coverage > 0.0f) {
                const float inner_distance =
                    DistToZ(px - 0.8f, py + 1.0f, 19.5f, -26.0f, 9.0f, 10.0f) -
                    1.6f;
                const float blend =
                    std::clamp((0.5f - inner_distance) / 1.2f, 0.0f, 1.0f);
                const uint32_t small_color =
                    BlendRgb(0x896a36, 0xc6a15b, SmoothStep(blend));
                color = BlendRgb(color, small_color, small_coverage);
                alpha = std::max(alpha, small_coverage);
            }

            if (alpha > 0.0f) {
                sleepy_zz_raster_.pixels[y * SleepyZzRaster::kWidth + x] =
                    (static_cast<uint32_t>(alpha * 255.0f) << 24) | color;
            }
        }
    }
    return true;
}

void MochanDisplay::AdvanceSleepyZzAnimation(bool visual_eligible) {
    if (sleepy_zz_ == nullptr || sleepy_zz_raster_.pixels == nullptr) {
        return;
    }

    const bool sleepy_active = face_state_ == FaceState::kSleepy;
    const bool sleepy_settled =
        std::abs(right_eye_geometry_.width - 72) <= 3 &&
        right_eye_geometry_.height <= 32 && right_eye_geometry_.bottom_curve > 0 &&
        right_eye_geometry_.slope == 0 && right_eye_geometry_.water == 0;

    if (!visual_eligible) {
        sleepy_zz_raster_.hold_ticks = 0;
        sleepy_zz_raster_.alpha = 0.0f;
    } else if (sleepy_active && sleepy_settled) {
        if (sleepy_zz_raster_.hold_ticks < 15) {
            ++sleepy_zz_raster_.hold_ticks;
        } else {
            sleepy_zz_raster_.alpha =
                std::min(1.0f, sleepy_zz_raster_.alpha + 0.018f);
        }
    } else {
        sleepy_zz_raster_.hold_ticks = 0;
        sleepy_zz_raster_.alpha =
            std::max(0.0f, sleepy_zz_raster_.alpha - 0.08f);
    }

    const uint8_t opacity = static_cast<uint8_t>(
        std::round(sleepy_zz_raster_.alpha * static_cast<float>(LV_OPA_COVER)));
    if (sleepy_zz_raster_.previous_opacity != opacity) {
        sleepy_zz_raster_.previous_opacity = opacity;
        lv_obj_set_style_opa(sleepy_zz_, opacity, 0);
    }
    if (opacity == 0) {
        if (!lv_obj_has_flag(sleepy_zz_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(sleepy_zz_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const int display_x = right_eye_geometry_.x + 24;
    const int display_y = right_eye_geometry_.y + face_layout_offset_y_ - 32;
    if (!sleepy_zz_raster_.positioned || sleepy_zz_raster_.displayed_x != display_x ||
        sleepy_zz_raster_.displayed_y != display_y) {
        sleepy_zz_raster_.positioned = true;
        sleepy_zz_raster_.displayed_x = display_x;
        sleepy_zz_raster_.displayed_y = display_y;
        lv_obj_align(sleepy_zz_, LV_ALIGN_CENTER, display_x, display_y);
    }
    if (lv_obj_has_flag(sleepy_zz_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(sleepy_zz_, LV_OBJ_FLAG_HIDDEN);
    }
}

void MochanDisplay::UpdateMouth(uint8_t blink_amount, const std::string& current_emotion) {
    if (mouth_ == nullptr || mouth_raster_.pixels == nullptr) {
        return;
    }
    std::string emotion = current_emotion;
    if (face_layout_target_ == 0 && !exiting_mouth_emotion_.empty()) {
        emotion = exiting_mouth_emotion_;
    }

    const auto* geometry = FindMouthGeometry(emotion);
    auto& state = mouth_morph_state_;
    if (geometry == nullptr || face_layout_progress_ == 0) {
        if (!lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        }
        if (face_layout_progress_ == 0) {
            exiting_mouth_emotion_.clear();
            mouth_shape_opacity_ = 0;
            state.initialized = false;
        }
        return;
    }

    mouth_shape_opacity_ = std::min(256, mouth_shape_opacity_ + 48);

    if (!state.initialized) {
        state.width = geometry->width;
        state.height = geometry->height;
        state.top_curve = geometry->top_curve;
        state.bottom_curve = geometry->bottom_curve;
        state.slope = geometry->slope;
        state.gap = geometry->gap;
        state.initialized = true;
        RenderMouthSdf(mouth_raster_.pixels, &mouth_raster_.descriptor,
                       MouthRaster::kWidth, MouthRaster::kHeight,
                       state.width, state.height, state.top_curve,
                       state.bottom_curve, state.slope);
        state.rendered_width = state.width;
        state.rendered_height = state.height;
        state.rendered_top_curve = state.top_curve;
        state.rendered_bottom_curve = state.bottom_curve;
        state.rendered_slope = state.slope;
        lv_obj_invalidate(mouth_);
    } else {
        const auto approach_val = [](float current, float target) {
            const float delta = target - current;
            if (std::fabs(delta) <= 0.25f) {
                return target;
            }
            const float step = delta / 3.0f;
            return current + (std::fabs(step) >= 0.25f ? step : (delta > 0.0f ? 0.25f : -0.25f));
        };
        state.width = approach_val(state.width, geometry->width);
        state.height = approach_val(state.height, geometry->height);
        state.top_curve = approach_val(state.top_curve, geometry->top_curve);
        state.bottom_curve = approach_val(state.bottom_curve, geometry->bottom_curve);
        state.slope = approach_val(state.slope, geometry->slope);
        state.gap = approach_val(state.gap, geometry->gap);

        const bool shape_changed =
            (std::fabs(state.rendered_width - state.width) > 0.1f ||
             std::fabs(state.rendered_height - state.height) > 0.1f ||
             std::fabs(state.rendered_top_curve - state.top_curve) > 0.1f ||
             std::fabs(state.rendered_bottom_curve - state.bottom_curve) > 0.1f ||
             std::fabs(state.rendered_slope - state.slope) > 0.1f);

        if (shape_changed) {
            RenderMouthSdf(mouth_raster_.pixels, &mouth_raster_.descriptor,
                           MouthRaster::kWidth, MouthRaster::kHeight,
                           state.width, state.height,
                           state.top_curve, state.bottom_curve, state.slope);
            state.rendered_width = state.width;
            state.rendered_height = state.height;
            state.rendered_top_curve = state.top_curve;
            state.rendered_bottom_curve = state.bottom_curve;
            state.rendered_slope = state.slope;
            lv_obj_invalidate(mouth_);
        }
    }

    // Set pivot at ~40% of height so mouth deformation naturally extends downward
    const int pivot_y = MouthRaster::kHeight * 2 / 5;
    if (mouth_raster_.previous_pivot_y != pivot_y) {
        mouth_raster_.previous_pivot_y = pivot_y;
        lv_obj_set_style_transform_pivot_y(mouth_, pivot_y, 0);
    }

    int expression_scale_x = 256;
    int expression_deformation_y = 0;
    if (emotion == "surprised" || emotion == "shocked") {
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

    const int scale_y = std::max(150, deformation_y);
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

    // Follow the natural eye center including gaze micro-movements
    const int mouth_x = (left_eye_geometry_.x + right_eye_geometry_.x) / 2;
    const int eye_center_y = (left_eye_geometry_.y + right_eye_geometry_.y) / 2;
    const int mouth_gap = std::round(state.gap);
    const int mouth_y = eye_center_y + face_layout_offset_y_ + mouth_gap;
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
                RoundedDistance(px, py - center, half_width, half_height, radius);
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            if (coverage <= 0.0f) {
                raster.pixels[y * EyeRaster::kWidth + x] = 0;
                continue;
            }
            const float inner_distance =
                RoundedDistance(inner_x, py - inner_center, half_width,
                                inner_half_height, inner_radius);
            const float t = std::clamp((1.0f - inner_distance) / 2.0f, 0.0f, 1.0f);
            uint32_t color = BlendRgb(0x896a36, 0xc6a15b, SmoothStep(t));
            if (geometry.water > 0) {
                const float outer = geometry.x < 0 ? -u : u;
                const float spread = std::clamp((outer + 0.35f) / 1.35f, 0.0f, 1.0f);
                const float waterline =
                    bottom - geometry.water * openness * SmoothStep(spread);
                const float water_mix =
                    std::clamp((py - waterline + 1.0f) / 2.0f, 0.0f, 1.0f);
                color = BlendRgb(color, 0x80643b, water_mix);
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

bool MochanDisplay::AllowsAmbientGaze(FaceState state) {
    switch (state) {
        case FaceState::kLookLeft:
        case FaceState::kLookRight:
        case FaceState::kLookUp:
        case FaceState::kLookDown:
        case FaceState::kLookUpLeft:
        case FaceState::kLookUpRight:
        case FaceState::kLookDownLeft:
        case FaceState::kLookDownRight:
            return false;
        default:
            return true;
    }
}

AmbientGazePersonality MochanDisplay::ResolveAmbientGazePersonality(FaceState state) {
    switch (state) {
        case FaceState::kIdle:
            return AmbientGazePersonality::kNeutral;
        case FaceState::kListening:
            return AmbientGazePersonality::kListening;
        case FaceState::kThinking:
            return AmbientGazePersonality::kThinking;
        case FaceState::kSpeaking:
            return AmbientGazePersonality::kSpeaking;
        case FaceState::kHappy:
        case FaceState::kLaughing:
        case FaceState::kFunny:
        case FaceState::kLoving:
        case FaceState::kWinking:
        case FaceState::kSilly:
            return AmbientGazePersonality::kHappy;
        case FaceState::kCool:
        case FaceState::kConfident:
            return AmbientGazePersonality::kCool;
        case FaceState::kRelaxed:
        case FaceState::kDelicious:
            return AmbientGazePersonality::kRelaxed;
        case FaceState::kSleepy:
            return AmbientGazePersonality::kSleepy;
        case FaceState::kConfused:
            return AmbientGazePersonality::kConfused;
        case FaceState::kSuspicious:
            return AmbientGazePersonality::kSuspicious;
        case FaceState::kLookLeft:
        case FaceState::kLookRight:
        case FaceState::kLookUp:
        case FaceState::kLookDown:
        case FaceState::kLookUpLeft:
        case FaceState::kLookUpRight:
        case FaceState::kLookDownLeft:
        case FaceState::kLookDownRight:
            return AmbientGazePersonality::kSuppressed;
        default:
            return AmbientGazePersonality::kNeutral;
    }
}

void MochanDisplay::UpdateEyes(uint8_t blink_amount, bool ambient_visual_eligible) {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    struct EyeTarget {
        EyeGeometry geometry;
    };
    EyeTarget left{{74, 54, -48, -59, 0, 3, 0, 0}};
    EyeTarget right{{74, 54, 48, -59, 0, 3, 0, 0}};

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
            left.geometry = {76, 50, -47, -56 + gentle, 0, 3, -6, 0};
            right.geometry = {76, 50, 47, -56 + gentle, 0, 3, -6, 0};
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
            left.geometry = {72, 26, -46, -48 + gentle, 0, 7, 1, 0};
            right.geometry = {72, 26, 46, -48 + gentle, 0, 7, 1, 0};
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

    const bool gaze_eligible = ambient_visual_eligible && AllowsAmbientGaze(face_state_);
    if (!gaze_eligible) {
        ambient_gaze_x_ = 0;
        ambient_gaze_y_ = 0;
    } else {
        ambient_gaze_x_ +=
            std::clamp(static_cast<int>(ambient_gaze_target_x_ - ambient_gaze_x_), -1, 1);
        ambient_gaze_y_ +=
            std::clamp(static_cast<int>(ambient_gaze_target_y_ - ambient_gaze_y_), -1, 1);
        left.geometry.x += ambient_gaze_x_;
        right.geometry.x += ambient_gaze_x_;
        left.geometry.y += ambient_gaze_y_;
        right.geometry.y += ambient_gaze_y_;
    }

    if (mouth_motion_amount_ != 0) {
        const int positive_mouth_motion = std::max<int>(0, mouth_motion_amount_);
        const int mouth_reaction = positive_mouth_motion * 2 / 256;
        if (face_state_ == FaceState::kSurprised || face_state_ == FaceState::kShocked) {
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
