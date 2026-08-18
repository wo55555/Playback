#include "ClientCameraCapture.h"

#include "playback/replay/ReplaySession.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/minecraft_camera/CameraRegistry.h"
#include "mc/deps/minecraft_camera/components/CameraComponent.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/world/actor/Actor.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace playback::keyframe {

namespace {

constexpr float Pi               = 3.14159265358979323846f;
constexpr float RadiansPerDegree = Pi / 180.0f;
constexpr float DegreesPerRadian = 180.0f / Pi;

bool finite(::glm::vec3 const& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float dot(::glm::vec3 const& left, ::glm::vec3 const& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

::glm::vec3 normalized(::glm::vec3 value) {
    float const length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length <= 0.0001f) return {};
    return value / length;
}

bool validDirection(::glm::vec3 const& value) { return finite(value) && dot(value, value) > 0.0001f; }

bool validBasis(::glm::vec3 const& right, ::glm::vec3 const& up, ::glm::vec3 const& forward) {
    if (!validDirection(right) || !validDirection(up) || !validDirection(forward)) return false;
    auto const      normalizedRight   = normalized(right);
    auto const      normalizedUp      = normalized(up);
    auto const      normalizedForward = normalized(forward);
    constexpr float Tolerance         = 0.08f;
    return std::abs(dot(normalizedRight, normalizedUp)) < Tolerance
        && std::abs(dot(normalizedRight, normalizedForward)) < Tolerance
        && std::abs(dot(normalizedUp, normalizedForward)) < Tolerance;
}

struct CameraBasis {
    ::glm::vec3 right{};
    ::glm::vec3 up{};
    ::glm::vec3 forward{};
};

std::optional<CameraBasis> basisFromModelView(::glm::mat4x4 const& modelView) {
    auto const  viewRotation  = ::glm::mat3{modelView};
    auto const  cameraToWorld = ::glm::transpose(viewRotation);
    CameraBasis basis{
        cameraToWorld[0],
        cameraToWorld[1],
        -cameraToWorld[2],
    };
    if (!validBasis(basis.right, basis.up, basis.forward)) return std::nullopt;
    basis.right   = normalized(basis.right);
    basis.up      = normalized(basis.up);
    basis.forward = normalized(basis.forward);
    return basis;
}

std::optional<CameraBasis> basisFromOrientation(::glm::qua<float> orientation) {
    float const length = std::sqrt(
        orientation.w * orientation.w + orientation.x * orientation.x + orientation.y * orientation.y
        + orientation.z * orientation.z
    );
    if (!std::isfinite(length) || length <= 0.0001f) return std::nullopt;

    auto const  cameraToWorld = ::glm::mat3_cast(orientation / length);
    CameraBasis basis{
        cameraToWorld[0],
        cameraToWorld[1],
        -cameraToWorld[2],
    };
    if (!validBasis(basis.right, basis.up, basis.forward)) return std::nullopt;
    basis.right   = normalized(basis.right);
    basis.up      = normalized(basis.up);
    basis.forward = normalized(basis.forward);
    return basis;
}

std::optional<CameraBasis> basisFromCamera(::mce::Camera const& camera) {
    if (validBasis(camera.mRight.get(), camera.mUp.get(), camera.mForward.get())) {
        return CameraBasis{
            normalized(camera.mRight.get()),
            normalized(camera.mUp.get()),
            normalized(camera.mForward.get()),
        };
    }

    auto const  viewRotation  = ::glm::mat3{camera.mInverseViewMatrix.get()};
    auto const  cameraToWorld = viewRotation;
    CameraBasis basis{
        cameraToWorld[0],
        cameraToWorld[1],
        -cameraToWorld[2],
    };
    if (!validBasis(basis.right, basis.up, basis.forward)) return std::nullopt;
    basis.right   = normalized(basis.right);
    basis.up      = normalized(basis.up);
    basis.forward = normalized(basis.forward);
    return basis;
}

Actor* resolveCameraActor(ClientInstance& client) {
    if (auto* actor = client.getCameraActor()) return actor;
    if (auto* actor = replay::ReplaySession::getInstance().getReplayPlayer()) return actor;
    return client.getLocalPlayer();
}

MinecraftCamera::CameraComponent* resolveGameCamera(ClientInstance& client) {
    auto registry = client.getCameraRegistry();
    if (!registry) return nullptr;
    auto& gameCamera = registry->mGameCamera.get();
    if (!gameCamera) return nullptr;
    return gameCamera->tryGetComponent<MinecraftCamera::CameraComponent>().as_ptr();
}

float cameraFovDegrees(float fov) {
    if (!std::isfinite(fov) || fov <= 0.0f) return 70.0f;
    return fov <= Pi ? fov * DegreesPerRadian : fov;
}

void cameraVectors(float yaw, float pitch, ::glm::vec3& right, ::glm::vec3& up) {
    float const yawRadians   = yaw * RadiansPerDegree;
    float const pitchRadians = pitch * RadiansPerDegree;
    float const sinYaw       = std::sin(yawRadians);
    float const cosYaw       = std::cos(yawRadians);
    float const sinPitch     = std::sin(pitchRadians);
    float const cosPitch     = std::cos(pitchRadians);
    right                    = {-cosYaw, 0.0f, -sinYaw};
    up                       = {-sinYaw * sinPitch, cosPitch, cosYaw * sinPitch};
}

void anglesFromBasis(CameraBasis const& basis, float& yaw, float& pitch, float& roll) {
    yaw   = std::atan2(-basis.forward.x, basis.forward.z) * DegreesPerRadian;
    pitch = std::atan2(-basis.forward.y, std::hypot(basis.forward.x, basis.forward.z)) * DegreesPerRadian;

    ::glm::vec3 noRollRight;
    ::glm::vec3 noRollUp;
    cameraVectors(yaw, pitch, noRollRight, noRollUp);
    roll = std::atan2(dot(basis.right, noRollUp), dot(basis.right, noRollRight)) * DegreesPerRadian;
}

} // namespace

std::optional<CameraRenderState> captureClientCamera() noexcept {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;

    auto* actor = resolveCameraActor(*client);

    auto const* camera = static_cast<mce::Camera const*>(nullptr);
    if (auto* renderer = client->getLevelRenderer()) {
        auto const& playerRenderer = renderer->mLevelRendererPlayer.get();
        if (playerRenderer) camera = &playerRenderer->mWorldSpaceCamera.get();
    }
    if (!camera || !validBasis(camera->mRight.get(), camera->mUp.get(), camera->mForward.get())) {
        camera = &client->getCamera();
    }

    auto const* gameCamera = resolveGameCamera(*client);
    auto const  position =
        gameCamera && finite(gameCamera->mPosition.get()) ? gameCamera->mPosition.get() : camera->mPosition.get();
    if (!finite(position)) return std::nullopt;

    float const fov = gameCamera && std::isfinite(gameCamera->mFieldOfView) && gameCamera->mFieldOfView > 0.0f
                        ? cameraFovDegrees(gameCamera->mFieldOfView)
                        : cameraFovDegrees(camera->mFov);

    std::optional<CameraBasis> basis;
    if (gameCamera) {
        basis = basisFromOrientation(gameCamera->mOrientation.get());
        if (!basis) basis = basisFromModelView(gameCamera->mSavedModelView->_m.get());
    }
    if (!basis) basis = basisFromCamera(*camera);

    float yaw{};
    float pitch{};
    float roll{};
    if (basis) {
        anglesFromBasis(*basis, yaw, pitch, roll);
    } else {
        if (!actor) return std::nullopt;
        auto const rotation = actor->getRotation();
        if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y)) return std::nullopt;
        yaw   = rotation.y;
        pitch = rotation.x;
    }

    return CameraRenderState{position.x, position.y, position.z, yaw, pitch, roll, fov};
}

} // namespace playback::keyframe
