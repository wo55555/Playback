#include "ClientCameraCapture.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <cmath>

namespace playback::editor::keyframe {

namespace {

constexpr float Pi               = 3.14159265358979323846f;
constexpr float RadiansPerDegree = Pi / 180.0f;
constexpr float DegreesPerRadian = 180.0f / Pi;

bool finite(::glm::vec3 const& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool nearZero(::glm::vec3 const& value) {
    constexpr float Epsilon = 0.0001f;
    return std::abs(value.x) < Epsilon && std::abs(value.y) < Epsilon && std::abs(value.z) < Epsilon;
}

float dot(::glm::vec3 const& left, ::glm::vec3 const& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

void cameraVectors(float yaw, float pitch, ::glm::vec3& right, ::glm::vec3& up) {
    float const yawRadians   = yaw * RadiansPerDegree;
    float const pitchRadians = pitch * RadiansPerDegree;
    float const sinYaw       = std::sin(yawRadians);
    float const cosYaw       = std::cos(yawRadians);
    float const sinPitch     = std::sin(pitchRadians);
    float const cosPitch     = std::cos(pitchRadians);
    right                    = {cosYaw, 0.0f, sinYaw};
    up                       = {-sinYaw * sinPitch, cosPitch, cosYaw * sinPitch};
}

} // namespace

std::optional<CameraRenderState> captureClientCamera() noexcept {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;

    auto const& camera   = client->getCamera();
    auto        position = *camera.mPosition;
    auto        forward  = *camera.mForward;
    auto const  right    = *camera.mRight;
    float       fov      = camera.mFov;

    if (auto* player = client->getLocalPlayer()) {
        if (!finite(position) || nearZero(position)) {
            auto const head = player->getHeadPos();
            position        = {head.x, head.y, head.z};
        }
        if (!finite(forward) || nearZero(forward)) {
            auto const  rotation = player->getRotation();
            float const yaw      = rotation.y * RadiansPerDegree;
            float const pitch    = rotation.x * RadiansPerDegree;
            float const cosPitch = std::cos(pitch);
            forward              = {-std::sin(yaw) * cosPitch, -std::sin(pitch), std::cos(yaw) * cosPitch};
        }
    }

    if (!std::isfinite(fov) || fov <= 1.0f) fov = 90.0f;
    if (!finite(position) || !finite(forward) || !std::isfinite(fov)) return std::nullopt;

    float const yaw   = std::atan2(-forward.x, forward.z) * DegreesPerRadian;
    float const pitch = std::atan2(-forward.y, std::hypot(forward.x, forward.z)) * DegreesPerRadian;
    float       roll  = 0.0f;
    if (finite(right)) {
        ::glm::vec3 noRollRight;
        ::glm::vec3 noRollUp;
        cameraVectors(yaw, pitch, noRollRight, noRollUp);
        roll = std::atan2(dot(right, noRollUp), dot(right, noRollRight)) * DegreesPerRadian;
    }

    return CameraRenderState{
        position.x,
        position.y,
        position.z,
        yaw,
        pitch,
        roll,
        std::clamp(fov, 1.0f, 179.0f),
    };
}

} // namespace playback::editor::keyframe
