#pragma once

#include "MathTypes.h"

#include <optional>
#include <string>

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
    Vec3  position{0, 80, 0};
    float yaw{0.0f};
    float pitch{0.0f};
    float roll{0.0f};
    float fov{70.0f};

    CameraInterpolationType interpolationType{CameraInterpolationType::Smooth};
    Vec2                    bezierCtrl1{0.42f, 0.0f};
    Vec2                    bezierCtrl2{0.58f, 1.0f};
};

// Addresses one keyframe in a batch edit. `pose` is only filled when pasting.
struct KeyframeRef {
    std::string                   cameraId;
    int                           tick{};
    std::optional<CameraKeyframe> pose;
};

} // namespace playback::state::editing::model
