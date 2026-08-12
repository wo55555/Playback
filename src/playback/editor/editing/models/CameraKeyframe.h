#pragma once

#include "MathTypes.h"

#include <string>

namespace playback::editor::editing::model {

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
enum class CameraPathType : uint8_t { Linear = 0, CubicBezier, AutoSmooth, Hermite };
enum class CameraTransitionPreset : uint8_t { Custom = 0, LinearConstant, CinematicEase, ArcPushIn, ArcPullOut, OrbitPass, WhipPan, ZoomTransition };

struct CameraMotionSegment {
    CameraPathType pathType{CameraPathType::Linear};
    CameraTransitionPreset preset{CameraTransitionPreset::LinearConstant};
    Vec3 outControl{};
    Vec3 inControl{};
    bool useLookAlongPath{};
    float fovPeakOffset{};
};

struct CameraKeyframe {
    std::string id;
    int         tick{};

    Vec3  position{0, 80, 0};
    float yaw{0.0f};
    float pitch{0.0f};
    float roll{0.0f};
    float fov{90.0f};
    Color4 tint{1,1,1,1};

    CameraInterpolationType interpolationType{CameraInterpolationType::Smooth};
    Vec2 bezierCtrl1{0.42f, 0.0f};
    Vec2 bezierCtrl2{0.58f, 1.0f};
    CameraMotionSegment outgoingMotion{};
};

} // namespace playback::editor::editing::model
