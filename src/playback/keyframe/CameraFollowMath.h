#pragma once

#include "CameraRenderState.h"
#include "playback/state/editing/models/MathTypes.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace playback::keyframe {

inline constexpr float kDegToRad = 0.017453292519943295f;
inline constexpr float kRadToDeg = 57.295779513082320877f;

[[nodiscard]] inline float wrapFollowDegrees(float value) noexcept {
    if (!std::isfinite(value)) return 0.0f;
    float const wrapped = std::remainder(value, 360.0f);
    return wrapped == -180.0f ? 180.0f : wrapped;
}

// Minecraft Bedrock view forward direction: yaw 0 faces +Z, positive pitch looks down.
[[nodiscard]] inline state::editing::model::Vec3 forwardVector(float yawDeg, float pitchDeg) noexcept {
    float const yawRad   = yawDeg * kDegToRad;
    float const pitchRad = pitchDeg * kDegToRad;
    float const cosPitch = std::cos(pitchRad);
    return {-std::sin(yawRad) * cosPitch, -std::sin(pitchRad), std::cos(yawRad) * cosPitch};
}

// Returns {yawDeg, pitchDeg} aiming from `from` toward `to`, or nullopt when the two points coincide.
[[nodiscard]] inline std::optional<std::pair<float, float>>
aimAngles(state::editing::model::Vec3 const& from, state::editing::model::Vec3 const& to) noexcept {
    float const dx    = to.x - from.x;
    float const dy    = to.y - from.y;
    float const dz    = to.z - from.z;
    float const lenSq = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(lenSq) || lenSq < 1e-10f) return std::nullopt;
    float const horiz = std::sqrt(dx * dx + dz * dz);
    float const yaw   = wrapFollowDegrees(std::atan2(-dx, dz) * kRadToDeg);
    float const pitch = std::clamp(std::atan2(-dy, horiz) * kRadToDeg, -89.9f, 89.9f);
    return std::pair<float, float>{yaw, pitch};
}

struct CustomThirdPersonPoseResult {
    state::editing::model::Vec3 anchor;
    CameraRenderState           camera;
};

[[nodiscard]] inline CustomThirdPersonPoseResult computeCustomThirdPersonPose(
    CameraRenderState const&           subjectEye,
    state::editing::model::Vec3 const& offset,
    float                              distance,
    float                              yaw,
    float                              pitch,
    float                              roll,
    float                              fov,
    bool                               followRotation
) noexcept {
    using Vec3 = state::editing::model::Vec3;

    Vec3  anchor{subjectEye.x, subjectEye.y, subjectEye.z};
    float camYaw   = 0.0f;
    float camPitch = 0.0f;

    if (followRotation) {
        float const subjYawRad = subjectEye.yaw * kDegToRad;
        Vec3 const  subjFwd{-std::sin(subjYawRad), 0.0f, std::cos(subjYawRad)};
        Vec3 const  subjRight{-std::cos(subjYawRad), 0.0f, -std::sin(subjYawRad)};
        anchor.x += subjRight.x * offset.x + subjFwd.x * offset.z;
        anchor.y += offset.y;
        anchor.z += subjRight.z * offset.x + subjFwd.z * offset.z;
        camYaw   = wrapFollowDegrees(subjectEye.yaw + yaw);
        camPitch = std::clamp(subjectEye.pitch + pitch, -89.9f, 89.9f);
    } else {
        anchor.x += offset.x;
        anchor.y += offset.y;
        anchor.z += offset.z;
        camYaw   = wrapFollowDegrees(yaw);
        camPitch = std::clamp(pitch, -89.9f, 89.9f);
    }

    Vec3 const  fwd  = forwardVector(camYaw, camPitch);
    float const dist = std::isfinite(distance) ? std::max(0.0f, distance) : 0.0f;
    Vec3 const  camPos{anchor.x - fwd.x * dist, anchor.y - fwd.y * dist, anchor.z - fwd.z * dist};

    return {
        anchor,
        CameraRenderState{camPos.x, camPos.y, camPos.z, camYaw, camPitch, roll, fov},
    };
}

struct CapturedThirdPersonParams {
    state::editing::model::Vec3 offset{};
    float                       distance{};
    float                       yaw{};
    float                       pitch{};
    float                       roll{};
    float                       fov{70.0f};
};

// Exact inverse of computeCustomThirdPersonPose for a live free camera relative to `subjectEye`.
[[nodiscard]] inline CapturedThirdPersonParams captureCustomThirdPersonFromFreeCamera(
    CameraRenderState const& freeCam,
    CameraRenderState const& subjectEye,
    bool                     followRotation
) noexcept {
    using Vec3 = state::editing::model::Vec3;

    Vec3 const  fwd = forwardVector(freeCam.yaw, freeCam.pitch);
    Vec3 const  delta{freeCam.x - subjectEye.x, freeCam.y - subjectEye.y, freeCam.z - subjectEye.z};
    float const dist = std::max(0.0f, -(delta.x * fwd.x + delta.y * fwd.y + delta.z * fwd.z));
    Vec3 const  worldOffset{delta.x + fwd.x * dist, delta.y + fwd.y * dist, delta.z + fwd.z * dist};

    Vec3  offset = worldOffset;
    float yaw    = wrapFollowDegrees(freeCam.yaw);
    float pitch  = std::clamp(freeCam.pitch, -89.9f, 89.9f);

    if (followRotation) {
        float const subjYawRad = subjectEye.yaw * kDegToRad;
        Vec3 const  subjFwd{-std::sin(subjYawRad), 0.0f, std::cos(subjYawRad)};
        Vec3 const  subjRight{-std::cos(subjYawRad), 0.0f, -std::sin(subjYawRad)};
        offset = {
            worldOffset.x * subjRight.x + worldOffset.z * subjRight.z,
            worldOffset.y,
            worldOffset.x * subjFwd.x + worldOffset.z * subjFwd.z,
        };
        yaw   = wrapFollowDegrees(freeCam.yaw - subjectEye.yaw);
        pitch = std::clamp(freeCam.pitch - subjectEye.pitch, -89.9f, 89.9f);
    }

    return {offset, dist, yaw, pitch, freeCam.roll, freeCam.fov};
}

struct ConvertedThirdPersonFrame {
    state::editing::model::Vec3 offset{};
    float                       yaw{};
    float                       pitch{};
};

// Converts (offset, yaw, pitch) between subject-relative and world-space orbit while preserving the world view.
[[nodiscard]] inline ConvertedThirdPersonFrame convertThirdPersonFollowRotation(
    CameraRenderState const&           subjectEye,
    state::editing::model::Vec3 const& offset,
    float                              yaw,
    float                              pitch,
    bool                               fromFollowRotation,
    bool                               toFollowRotation
) noexcept {
    using Vec3 = state::editing::model::Vec3;

    if (fromFollowRotation == toFollowRotation) return {offset, yaw, pitch};

    float const subjYawRad = subjectEye.yaw * kDegToRad;
    Vec3 const  subjFwd{-std::sin(subjYawRad), 0.0f, std::cos(subjYawRad)};
    Vec3 const  subjRight{-std::cos(subjYawRad), 0.0f, -std::sin(subjYawRad)};

    if (fromFollowRotation) {
        Vec3 const worldOffset{
            subjRight.x * offset.x + subjFwd.x * offset.z,
            offset.y,
            subjRight.z * offset.x + subjFwd.z * offset.z,
        };
        return {
            worldOffset,
            wrapFollowDegrees(subjectEye.yaw + yaw),
            std::clamp(subjectEye.pitch + pitch, -89.9f, 89.9f),
        };
    }

    Vec3 const relOffset{
        offset.x * subjRight.x + offset.z * subjRight.z,
        offset.y,
        offset.x * subjFwd.x + offset.z * subjFwd.z,
    };
    return {
        relOffset,
        wrapFollowDegrees(yaw - subjectEye.yaw),
        std::clamp(pitch - subjectEye.pitch, -89.9f, 89.9f),
    };
}

} // namespace playback::keyframe
