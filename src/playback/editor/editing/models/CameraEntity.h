#pragma once

#include "CameraKeyframe.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::editor::editing::model {

enum class CameraKind : uint8_t { Keyframe = 0, Path, Rig, Preset };
enum class SplineType : uint8_t { Linear = 0, CatmullRom, CubicBezier };
enum class RigMotion : uint8_t { Dolly = 0, Truck, Pedestal, Pan, Tilt, Roll, Zoom, Follow };
enum class PresetKind : uint8_t { FirstPerson = 0, ThirdPerson, Free, FollowEntity, Orbit, Telephoto, Drone };

struct CameraPathPoint { int tick{}; Vec3 position{}; Vec3 inTangent{}; Vec3 outTangent{}; };
struct CameraPath { std::vector<CameraPathPoint> points; SplineType type{SplineType::Linear}; Vec2 defaultRotation{}; float defaultFov{90.0f}; };
struct CameraRigSegment { RigMotion motion{RigMotion::Dolly}; int startTick{}; int endTick{}; float startValue{}; float endValue{}; EasingType easing{EasingType::Linear}; };
struct CameraRig { Vec3 basePosition{0, 80, 0}; Vec2 baseRotation{}; float baseFov{90.0f}; std::vector<CameraRigSegment> segments; };
struct CameraPreset {
    PresetKind kind{PresetKind::Free};
    Vec3       offset{};
    Vec2       rotation{};
    float      fov{90.0f};
    Vec3       orbitCenter{};
    float      orbitRadius{4.0f};
    float      orbitSpeed{1.0f};
    int        orbitPhaseTick{};
};
struct CameraShake { int startTick{}; int endTick{}; float positionAmplitude{}; float rotationAmplitude{}; float frequency{1.0f}; };
struct CameraLimiter { Vec3 min{}; Vec3 max{}; bool enabled{}; };

struct CameraEntity {
    std::string id;
    std::string name;
    CameraKind kind{CameraKind::Keyframe};
    // CameraEntity is also the canonical camera-track model. Runtime
    // evaluation, path visualization, and editing state stay beside the data
    // they control instead of being mirrored into a second track collection.
    bool enabled{true};
    bool pathVisible{true};
    std::vector<CameraKeyframe> keys;
    std::optional<CameraPath> path;
    std::optional<CameraRig> rig;
    std::optional<CameraPreset> preset;
    std::optional<CameraShake> shake;
    std::optional<CameraLimiter> limiter;
    std::string bindingEntityUuid;
    int bindingMode{};
    float bindingDamping{0.1f};
    bool locked{};
};

[[nodiscard]] inline bool hasCameraSource(CameraEntity const& camera, CameraKind kind) noexcept {
    switch (kind) {
    case CameraKind::Keyframe:
        return !camera.keys.empty();
    case CameraKind::Path:
        return camera.path && !camera.path->points.empty();
    case CameraKind::Rig:
        return camera.rig.has_value();
    case CameraKind::Preset:
        return camera.preset.has_value();
    }
    return false;
}

[[nodiscard]] inline bool isCameraRenderable(CameraEntity const& camera) noexcept {
    return camera.enabled && hasCameraSource(camera, camera.kind);
}

} // namespace playback::editor::editing::model
