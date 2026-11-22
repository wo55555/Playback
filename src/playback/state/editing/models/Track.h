#pragma once

#include "CameraKeyframe.h"
#include "MathTypes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace playback::state::editing::model {

inline constexpr float MinSpeedKeyframeValue = 0.01f;
inline constexpr float MaxSpeedKeyframeValue = 32.0f;

struct SpeedKeyframe {
    float                   speed{1.0f};
    CameraInterpolationType interpolationType{CameraInterpolationType::Smooth};
    Vec2                    bezierCtrl1{0.42f, 0.0f};
    Vec2                    bezierCtrl2{0.58f, 1.0f};
};

struct SpeedTrack {
    bool                         present{false};
    bool                         enabled{true};
    bool                         locked{false};
    std::map<int, SpeedKeyframe> keysByTick;

    [[nodiscard]] bool isVisible() const noexcept { return present || !keysByTick.empty(); }
};

[[nodiscard]] inline float clampSpeedKeyframeValue(float speed) noexcept {
    if (!std::isfinite(speed)) return 1.0f;
    return std::clamp(speed, MinSpeedKeyframeValue, MaxSpeedKeyframeValue);
}

struct Clip {
    std::string id;
    std::string replayFile;
    int         inTick{};
    int         outTick{};
    int         trackTick{};
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

[[nodiscard]] inline int clipSourceDurationTicks(Clip const& clip) noexcept {
    return std::max(0, clip.outTick - clip.inTick);
}

} // namespace playback::state::editing::model
