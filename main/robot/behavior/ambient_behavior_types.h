#pragma once

#include <cstdint>

enum class AmbientGazePersonality : uint8_t {
    kNeutral,
    kListening,
    kThinking,
    kSpeaking,
    kHappy,
    kCool,
    kRelaxed,
    kSleepy,
    kConfused,
    kSuspicious,
    kSuppressed,
};

enum class AmbientSoundCue : uint8_t {
    kNone,
    kRelaxed,
    kCurious,
    kPlayful,
    kSleepy,
};
