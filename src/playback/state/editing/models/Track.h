#pragma once

#include "MathTypes.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace playback::state::editing::model {

constexpr float MinClipSpeed = 0.05f;
constexpr float MaxClipSpeed = 32.0f;

struct Clip {
    std::string id;
    std::string replayFile;
    int         inTick{};
    int         outTick{};
    int         trackTick{};
    float       speed{1.0f};
    std::string name;
    Color4      color{0, 0, 1, 1};
    bool        muted{false};
    bool        locked{false};
    bool        exportEnabled{true};
};

enum class TransitionKind : uint8_t { Cut = 0, Fade, CrossDissolve };

struct Transition {
    std::string    id;
    TransitionKind kind{TransitionKind::Cut};
    int            durationTicks{20};
    int            easing{0};
    std::string    fromClipId;
    std::string    toClipId;
    Color4         fadeColor{0, 0, 0, 1};

    [[nodiscard]] float blendAlpha(int tickInTransition) const;
};

struct Marker {
    std::string id;
    std::string label;
    int         tick{};
};

inline float Transition::blendAlpha(int tickInTransition) const {
    if (durationTicks <= 0) return 1.0f;
    float t = static_cast<float>(tickInTransition) / static_cast<float>(durationTicks);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t;
}

[[nodiscard]] inline float clampClipSpeed(float speed) noexcept {
    if (!std::isfinite(speed)) return 1.0f;
    return std::clamp(speed, MinClipSpeed, MaxClipSpeed);
}

[[nodiscard]] inline int clipSourceDurationTicks(Clip const& clip) noexcept {
    return std::max(0, clip.outTick - clip.inTick);
}

[[nodiscard]] inline double clipOutputDurationSeconds(Clip const& clip) noexcept {
    return static_cast<double>(clipSourceDurationTicks(clip))
         / (20.0 * static_cast<double>(clampClipSpeed(clip.speed)));
}

} // namespace playback::state::editing::model
