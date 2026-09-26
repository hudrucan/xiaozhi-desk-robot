#pragma once

#include <cstdint>
#include <string_view>

enum class VoiceInputProfile : uint8_t {
    kLegacy,
    kNear,
    kBalanced,
    kFar,
    kNoisy,
    kCustom,
};

struct VoiceInputConfig {
    VoiceInputProfile profile = VoiceInputProfile::kLegacy;
    int capture_trim_db = 0;
    int voice_gain_db = 0;
    bool ns_requested = false;
    bool agc_requested = false;
};

struct PcmLevelStatus {
    bool valid = false;
    float rms_dbfs = -90.0f;
    float peak_dbfs = -90.0f;
    int64_t last_update_us = 0;
};

struct VoiceInputStatus {
    VoiceInputConfig config;
    bool ns_available = false;
    bool ns_active = false;
    bool agc_available = false;
    bool agc_active = false;
    bool restart_required = false;
    PcmLevelStatus voice_level;
    PcmLevelStatus afe_output_level;
};

inline const char* VoiceInputProfileName(VoiceInputProfile profile) {
    switch (profile) {
        case VoiceInputProfile::kNear:
            return "near";
        case VoiceInputProfile::kBalanced:
            return "balanced";
        case VoiceInputProfile::kFar:
            return "far";
        case VoiceInputProfile::kNoisy:
            return "noisy";
        case VoiceInputProfile::kCustom:
            return "custom";
        case VoiceInputProfile::kLegacy:
        default:
            return "legacy";
    }
}

inline VoiceInputProfile ParseVoiceInputProfile(std::string_view name) {
    if (name == "near") return VoiceInputProfile::kNear;
    if (name == "balanced") return VoiceInputProfile::kBalanced;
    if (name == "far") return VoiceInputProfile::kFar;
    if (name == "noisy") return VoiceInputProfile::kNoisy;
    if (name == "custom") return VoiceInputProfile::kCustom;
    return VoiceInputProfile::kLegacy;
}

inline VoiceInputConfig VoiceInputProfileDefaults(VoiceInputProfile profile) {
    VoiceInputConfig config;
    config.profile = profile;
    switch (profile) {
        case VoiceInputProfile::kNear:
            config.voice_gain_db = -6;
            config.ns_requested = true;
            break;
        case VoiceInputProfile::kBalanced:
            config.ns_requested = true;
            break;
        case VoiceInputProfile::kFar:
            config.voice_gain_db = 6;
            config.ns_requested = true;
            break;
        case VoiceInputProfile::kNoisy:
            config.voice_gain_db = -3;
            config.ns_requested = true;
            break;
        case VoiceInputProfile::kCustom:
        case VoiceInputProfile::kLegacy:
        default:
            break;
    }
    return config;
}
