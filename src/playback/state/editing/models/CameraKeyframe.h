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

enum class CameraKeyframeMode : uint8_t {
    Interpolated = 0,
    FirstPerson  = 1,
    ThirdPerson  = 2,
};

struct CameraKeyframe {
    Vec3  position{0, 80, 0};
    float yaw{0.0f};
    float pitch{0.0f};
    float roll{0.0f};
    float fov{70.0f};

    CameraKeyframeMode      mode{CameraKeyframeMode::Interpolated};
    CameraInterpolationType interpolationType{CameraInterpolationType::Smooth};
    Vec2                    bezierCtrl1{0.42f, 0.0f};
    Vec2                    bezierCtrl2{0.58f, 1.0f};

    // First- and third-person keyframes only: the replay player or entity viewed from this key on. Empty means the
    // recorder.
    std::string subject;
    // Interpolated and custom third-person keyframes: advance in real output time instead of scaling with clip speed.
    bool ignoreClipSpeed{false};

    // Third-person keyframes: false = legacy fixed vanilla F5; true = custom interpolated orbit/offset camera.
    bool  thirdPersonCustom{false};
    float thirdPersonDistance{4.0f};
    float thirdPersonYaw{0.0f};
    float thirdPersonPitch{0.0f};
    float thirdPersonRoll{0.0f};
    float thirdPersonFov{70.0f};
    Vec3  thirdPersonOffset{0.0f, 0.0f, 0.0f};
    bool  thirdPersonFollowRotation{false};
    bool  thirdPersonCollideBlocks{true};

    // Interpolated keyframes only: optionally aim yaw/pitch at a replay player or entity.
    bool        lookAtEnabled{false};
    std::string lookAtTarget; // empty means the recorder when lookAtEnabled is true
    float       lookAtOffsetY{0.0f};
};

// Addresses one keyframe in a batch edit. `pose` is only filled when pasting.
struct KeyframeRef {
    std::string                   cameraId;
    int                           tick{};
    std::optional<CameraKeyframe> pose;
};

} // namespace playback::state::editing::model
