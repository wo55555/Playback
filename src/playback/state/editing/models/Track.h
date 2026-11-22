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

inline constexpr int MinFreezeDelayTicks = 0;
inline constexpr int MaxFreezeDelayTicks = 10;

struct FreezeKeyframe {
    bool frozen{true};
    int  frozenDelay{0};

    bool operator==(FreezeKeyframe const&) const = default;
};

struct FreezeTrack {
    bool                          present{false};
    bool                          enabled{true};
    bool                          locked{false};
    std::map<int, FreezeKeyframe> keysByTick;

    bool operator==(FreezeTrack const&) const = default;

    [[nodiscard]] bool isVisible() const noexcept { return present || !keysByTick.empty(); }
};

[[nodiscard]] inline int clampFreezeDelayTicks(int delayTicks) noexcept {
    return std::clamp(delayTicks, MinFreezeDelayTicks, MaxFreezeDelayTicks);
}

inline constexpr int    MinTimelapseOutputTicks = 0;
inline constexpr int    MaxTimelapseOutputTicks = 20 * 60 * 60 * 24;
inline constexpr double MinTimelapseSpeed       = 0.001;
inline constexpr double MaxTimelapseSpeed       = 10000.0;

struct TimelapseKeyframe {
    int outputTicks{0};

    bool operator==(TimelapseKeyframe const&) const = default;
};

struct TimelapseTrack {
    bool                             present{false};
    bool                             enabled{true};
    bool                             locked{false};
    std::map<int, TimelapseKeyframe> keysByTick;

    bool operator==(TimelapseTrack const&) const = default;

    [[nodiscard]] bool isVisible() const noexcept { return present || !keysByTick.empty(); }
};

[[nodiscard]] inline int clampTimelapseOutputTicks(int ticks) noexcept {
    return std::clamp(ticks, MinTimelapseOutputTicks, MaxTimelapseOutputTicks);
}

struct Clip {
    std::string id;
    std::string replayFile;
    int         inTick{};
    int         outTick{};
    int         trackTick{};
    std::string name;
    Color4      color{0, 0, 1, 1};
    bool        locked{false};
    bool        exportEnabled{true};
};

inline constexpr float  MinAudioVolume   = 0.0f;
inline constexpr float  MaxAudioVolume   = 2.0f;
inline constexpr double MinAudioTrimSecs = 0.0;

struct AudioClip {
    std::string id;
    std::string name;
    std::string filePath;
    int         startTick{0};
    double      trimStartSeconds{0.0};
    double      durationSeconds{0.0}; // 0 means full remaining duration of the audio file
    float       volume{1.0f};
    bool        muted{false};
};

struct AudioTrack {
    std::string            id;
    std::string            name;
    bool                   enabled{true};
    bool                   muted{false};
    bool                   locked{false};
    float                  volume{1.0f};
    std::vector<AudioClip> clips;
};

[[nodiscard]] inline float clampAudioVolume(float volume) noexcept {
    if (!std::isfinite(volume)) return 1.0f;
    return std::clamp(volume, MinAudioVolume, MaxAudioVolume);
}

[[nodiscard]] inline double clampAudioSeconds(double seconds) noexcept {
    if (!std::isfinite(seconds)) return 0.0;
    return std::max(MinAudioTrimSecs, seconds);
}

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
