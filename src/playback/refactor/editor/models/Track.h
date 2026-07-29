#pragma once

#include "CameraKeyframe.h"

#include <string>
#include <vector>

namespace playback::refactor::editor {

// ===== TrackKind =====
enum class TrackKind : uint8_t {
    Video = 0,
    Camera,
    Marker
};

// ===== Clip =====
struct Clip {
    std::string id;               // uuid
    std::string replayFile;       // .playback 路径
    int         inTick{};         // 原始回放内的起始 tick
    int         outTick{};        // 原始回放内的结束 tick
    int         trackTick{};      // 在 track 上的起始 tick
    int         activeCameraTrackIdx{0};
    float       speed{1.0f};      // 局部变速
    std::string name;
    Color4      color{Color4::Blue};
    bool        muted{false};
    bool        locked{false};
};

// ===== Transition =====
enum class TransitionKind : uint8_t {
    Cut = 0,           // 硬切（duration=0）
    Fade,              // 淡入淡出
    CrossDissolve      // 交叉溶解
};

struct Transition {
    std::string    id;
    TransitionKind kind{TransitionKind::Cut};
    int            durationTicks{20};   // 0=Cut
    int            easing{0};           // EasingType index
    std::string    fromClipId;
    std::string    toClipId;
    Color4         fadeColor{Color4::Black};  // Fade 用

    [[nodiscard]] float blendAlpha(int tickInTransition) const;
};

// ===== Track =====
struct Track {
    std::string        id;
    std::string        name;
    TrackKind          kind{TrackKind::Video};
    std::vector<Clip>  clips;
    bool               visible{true};
    bool               locked{false};
    int                height{48};       // UI 像素高度
};

// ===== Camera track (Cn) =====
struct CameraTrack {
    std::string              id;
    std::string              name;
    bool                     active{};
    bool                     locked{};
    bool                     muted{};
    bool                     visible{true};
    std::vector<CameraKeyframe> keyframes;
};

// ===== Marker =====
struct Marker {
    std::string id;
    std::string label;
    int         tick{};
};

// ===== TrackType (UI) =====
enum class TrackType {
    Video,      // V0
    Camera,     // Cn
    Markers     // M
};

// ===== TrackDescriptor (UI) =====
struct TrackDescriptor {
    TrackType type{TrackType::Camera};
    std::string id;
    std::string name;
    bool        active{};
    bool        locked{};
    bool        muted{};
    bool        visible{true};
};

// ===== Inline implementations =====
inline float Transition::blendAlpha(int tickInTransition) const {
    if (durationTicks <= 0) return 1.0f;
    float t = static_cast<float>(tickInTransition) / static_cast<float>(durationTicks);
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    // Simple linear easing for now
    return t;
}

} // namespace playback::refactor::editor