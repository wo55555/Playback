#pragma once

#include <string>

namespace playback::refactor::editor {

struct Vec3 {
    float x{}, y{}, z{};
};

struct Vec2 {
    float x{}, y{};
};

struct Color4 {
    float r{1}, g{1}, b{1}, a{1};
};

// A single keyframe at a specific tick for a camera track
struct CameraKeyframe {
    std::string id;
    int         tick{};

    Vec3  position{0, 80, 0};
    Vec3  rotation{0, 0, 0}; // yaw, pitch, roll
    float fov{90.0f};

    // Easing type: 0=Linear, 1=EaseIn, 2=EaseOut, 3=Cubic, 4=Custom
    int easing{0};
};

} // namespace playback::refactor::editor