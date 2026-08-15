#pragma once

#include "MathTypes.h"

namespace playback::state::editing::model {

enum class CameraSidedInterpolationType : uint8_t { Smooth, Linear, Ease, Hold, Hermite, CubicBezier };
enum class CameraInterpolationType : uint8_t {
    Smooth = 0,
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    Hold,
    Hermite,
    CubicBezier,
};

struct CameraKeyframe {
    Vec3   position{0, 80, 0};
    float  yaw{0.0f};
    float  pitch{0.0f};
    float  roll{0.0f};
    float  fov{70.0f};
    Color4 tint{1, 1, 1, 1};

    CameraInterpolationType interpolationType{CameraInterpolationType::Smooth};
    Vec2                    bezierCtrl1{0.42f, 0.0f};
    Vec2                    bezierCtrl2{0.58f, 1.0f};
};

} // namespace playback::state::editing::model
