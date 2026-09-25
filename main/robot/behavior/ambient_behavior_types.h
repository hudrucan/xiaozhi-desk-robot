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
