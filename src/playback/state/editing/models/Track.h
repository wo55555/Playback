#pragma once

#include "MathTypes.h"

#include <string>
#include <vector>

namespace playback::state::editing::model {

enum class TrackKind : uint8_t { Video = 0, Camera, Marker };

struct Clip {
    std::string id;
    std::string replayFile;
    int         inTick{};
    int         outTick{};
    int         trackTick{};
    int         activeCameraTrackIdx{0};
    float       speed{1.0f};
    std::string name;
    Color4      color{0, 0, 1, 1};
    bool        muted{false};
    bool        locked{false};
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

struct Track {
    std::string       id;
    std::string       name;
    TrackKind         kind{TrackKind::Video};
    std::vector<Clip> clips;
    bool              visible{true};
    bool              locked{false};
    int               height{48};
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

} // namespace playback::state::editing::model
