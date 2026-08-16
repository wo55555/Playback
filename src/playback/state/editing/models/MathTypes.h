#pragma once

#include <cstdint>

namespace playback::state::editing::model {

struct Vec3 {
    float x{}, y{}, z{};
};

struct Vec2 {
    float x{}, y{};
};

struct Color4 {
    float r{1}, g{1}, b{1}, a{1};

    static Color4 Black() { return {0, 0, 0, 1}; }
    static Color4 White() { return {1, 1, 1, 1}; }
    static Color4 Blue() { return {0, 0, 1, 1}; }
    static Color4 Red() { return {1, 0, 0, 1}; }
    static Color4 Green() { return {0, 1, 0, 1}; }
};

enum class EasingType : uint8_t { Linear = 0, EaseIn, EaseOut, EaseInOut, CubicBezier, Hold };

} // namespace playback::state::editing::model
