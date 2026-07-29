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

    static Color4 Black()  { return {0,0,0,1}; }
    static Color4 White()  { return {1,1,1,1}; }
    static Color4 Blue()   { return {0,0,1,1}; }
    static Color4 Red()    { return {1,0,0,1}; }
    static Color4 Green()  { return {0,1,0,1}; }
};

// Easing type enum
enum class EasingType : uint8_t {
    Linear = 0,
    EaseIn,
    EaseOut,
    EaseInOut
};

// A single keyframe at a specific tick for a camera track
struct CameraKeyframe {
    std::string id;
    int         tick{};

    Vec3  position{0, 80, 0};
    float yaw{0.0f};
    float pitch{0.0f};
    float fov{90.0f};
    Color4 tint{1,1,1,1};

    // Easing type
    EasingType easingType{EasingType::Linear};
};

} // namespace playback::refactor::editor