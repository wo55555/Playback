#pragma once

#include "MathTypes.h"

#include <string>
#include <vector>

namespace playback::state::editing::model {

// ===== TrackKind =====
enum class TrackKind : uint8_t { Video = 0, Camera, Marker };

// ===== Clip =====
struct Clip {
    std::string id;          // uuid
    std::string replayFile;  // .playback 璺緞
    int         inTick{};    // 鍘熷鍥炴斁鍐呯殑璧峰 tick
    int         outTick{};   // 鍘熷鍥炴斁鍐呯殑缁撴潫 tick
    int         trackTick{}; // 鍦?track 涓婄殑璧峰 tick
    int         activeCameraTrackIdx{0};
    float       speed{1.0f}; // 灞€閮ㄥ彉閫?
    std::string name;
    Color4      color{0, 0, 1, 1};
    bool        muted{false};
    bool        locked{false};
};

// ===== Transition =====
enum class TransitionKind : uint8_t {
    Cut = 0,      // 纭垏锛坉uration=0锛?
    Fade,         // 娣″叆娣″嚭
    CrossDissolve // 浜ゅ弶婧惰В
};

struct Transition {
    std::string    id;
    TransitionKind kind{TransitionKind::Cut};
    int            durationTicks{20}; // 0=Cut
    int            easing{0};         // EasingType index
    std::string    fromClipId;
    std::string    toClipId;
    Color4         fadeColor{0, 0, 0, 1}; // Fade 鐢?

    [[nodiscard]] float blendAlpha(int tickInTransition) const;
};

// ===== Track =====
struct Track {
    std::string       id;
    std::string       name;
    TrackKind         kind{TrackKind::Video};
    std::vector<Clip> clips;
    bool              visible{true};
    bool              locked{false};
    int               height{48}; // UI 鍍忕礌楂樺害
};

// ===== Marker =====
struct Marker {
    std::string id;
    std::string label;
    int         tick{};
};

// ===== Inline implementations =====
inline float Transition::blendAlpha(int tickInTransition) const {
    if (durationTicks <= 0) return 1.0f;
    float t = static_cast<float>(tickInTransition) / static_cast<float>(durationTicks);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t;
}

} // namespace playback::state::editing::model
