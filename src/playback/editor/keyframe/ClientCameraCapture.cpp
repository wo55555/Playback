#include "ClientCameraCapture.h"

#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/world/actor/Actor.h"

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

::glm::vec3 normalized(::glm::vec3 value) {
    float const length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length <= 0.0001f) return {};
    return value / length;
}

bool validDirection(::glm::vec3 const& value) {
    float const length = std::sqrt(dot(value, value));
    return finite(value) && std::isfinite(length) && length > 0.0001f;
}

bool validBasis(::glm::vec3 const& right, ::glm::vec3 const& up, ::glm::vec3 const& forward) {
    if (!validDirection(right) || !validDirection(up) || !validDirection(forward)) return false;
    auto const normalizedRight   = normalized(right);
    auto const normalizedUp      = normalized(up);
    auto const normalizedForward = normalized(forward);
    constexpr float BasisTolerance = 0.08f;
    return std::abs(dot(normalizedRight, normalizedUp)) < BasisTolerance
        && std::abs(dot(normalizedRight, normalizedForward)) < BasisTolerance
        && std::abs(dot(normalizedUp, normalizedForward)) < BasisTolerance;
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

::glm::vec3 directionFromRotation(float yaw, float pitch) {
    float const yawRadians   = yaw * RadiansPerDegree;
    float const pitchRadians = pitch * RadiansPerDegree;
    float const cosPitch     = std::cos(pitchRadians);
    return {
        -std::sin(yawRadians) * cosPitch,
        -std::sin(pitchRadians),
        std::cos(yawRadians) * cosPitch,
    };
}

bool actorViewBasis(
    Actor const& actor,
    ::glm::vec3& forward,
    float&        yaw,
    float&        pitch,
    float&        headYaw
) {
    auto const rotation = actor.getRotation();
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y)) return false;

    pitch   = rotation.x;
    yaw     = rotation.y;
    headYaw = yaw;
    auto& context = actor.getEntityContext();
    if (auto const head = context.tryGetComponent<ActorHeadRotationComponent>().as_ptr()) {
        if (std::isfinite(head->mYHeadRot)) headYaw = head->mYHeadRot;
    }

    auto const actorView = actor.getViewVector(1.0f);
    forward               = {actorView.x, actorView.y, actorView.z};
    if (!validDirection(forward)) forward = directionFromRotation(yaw, pitch);
    return validDirection(forward);
}

::glm::vec3 inverseViewForward(::glm::mat4x4 const& inverseView) {
    return {-inverseView[2].x, -inverseView[2].y, -inverseView[2].z};
}

::glm::vec3 inverseViewRight(::glm::mat4x4 const& inverseView) {
    return {inverseView[0].x, inverseView[0].y, inverseView[0].z};
}

::glm::vec3 inverseViewUp(::glm::mat4x4 const& inverseView) {
    return {inverseView[1].x, inverseView[1].y, inverseView[1].z};
}

} // namespace

std::optional<ClientCameraCapture> captureClientCamera() noexcept {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;

    Actor* const  cameraActor = client->getCameraActor();
    Player* const replayPlayer = functions::ReplaySession::getInstance().getReplayPlayer();
    Actor*        actor        = cameraActor;
    if (!actor) actor = replayPlayer;
    if (!actor) actor = client->getLocalPlayer();

    auto const* camera          = &client->getCamera();
    bool        usedWorldCamera = false;
    if (!validBasis(camera->mRight.get(), camera->mUp.get(), camera->mForward.get())) {
        if (auto* levelRenderer = client->getLevelRenderer()) {
            auto const& levelRendererPlayer = levelRenderer->mLevelRendererPlayer.get();
            if (levelRendererPlayer) {
                auto const& worldCamera = levelRendererPlayer->mWorldSpaceCamera.get();
                if (validBasis(worldCamera.mRight.get(), worldCamera.mUp.get(), worldCamera.mForward.get())) {
                    camera          = &worldCamera;
                    usedWorldCamera = true;
                }
            }
        }
    }

    auto const nativeForward = camera->mForward.get();
    auto const nativeRight   = camera->mRight.get();
    auto const nativeUp      = camera->mUp.get();

    ::glm::vec3 cameraPosition{};
    bool        usedWorldPosition = false;
    if (auto* levelRenderer = client->getLevelRenderer()) {
        auto const& levelRendererPlayer = levelRenderer->mLevelRendererPlayer.get();
        if (levelRendererPlayer) {
            auto const worldPosition = levelRendererPlayer->mWorldSpaceCamera.get().mPosition.get();
            if (finite(worldPosition) && !nearZero(worldPosition)) {
                cameraPosition     = worldPosition;
                usedWorldPosition   = true;
            }
        }
    }
    if (!usedWorldPosition) {
        auto const clientPosition = client->getCamera().mPosition.get();
        if (finite(clientPosition) && !nearZero(clientPosition)) cameraPosition = clientPosition;
    }
    if (nearZero(cameraPosition) && actor) {
        auto const head = actor->getHeadPos();
        cameraPosition  = {head.x, head.y, head.z};
    }
    if (!actor) return std::nullopt;
    auto const& actorPosition = actor->getPosition();
    ::glm::vec3 basePosition{actorPosition.x, actorPosition.y, actorPosition.z};
    if (!finite(basePosition) || !finite(cameraPosition)) return std::nullopt;

    auto       forward       = nativeForward;
    auto       right         = nativeRight;
    auto       up            = nativeUp;

    float       actorYaw     = 0.0f;
    float       actorPitch   = 0.0f;
    float       actorHeadYaw = 0.0f;
    ::glm::vec3 actorForward{};
    bool const  hasActorBasis =
        actor && actorViewBasis(*actor, actorForward, actorYaw, actorPitch, actorHeadYaw);

    bool usedNativeRotation = validBasis(right, up, forward);
    bool usedMatrixRotation = false;
    bool usedActorRotation  = false;

    if (!usedNativeRotation) {
        auto const& inverseView   = camera->mInverseViewMatrix.get();
        auto        matrixForward = inverseViewForward(inverseView);
        auto const  matrixRight   = inverseViewRight(inverseView);
        auto const  matrixUp      = inverseViewUp(inverseView);
        if (hasActorBasis && dot(matrixForward, actorForward) < 0.0f) matrixForward = -matrixForward;
        if (validBasis(matrixRight, matrixUp, matrixForward)) {
            forward            = matrixForward;
            right              = matrixRight;
            up                 = matrixUp;
            usedNativeRotation = true;
            usedMatrixRotation = true;
        }
    }

    if (!usedNativeRotation && hasActorBasis) {
        forward           = actorForward;
        cameraVectors(actorYaw, actorPitch, right, up);
        usedActorRotation = true;
    }
    if (!validDirection(forward)) return std::nullopt;

    forward    = normalized(forward);
    float yaw  = std::atan2(-forward.x, forward.z) * DegreesPerRadian;
    float pitch = std::atan2(-forward.y, std::hypot(forward.x, forward.z)) * DegreesPerRadian;
    float roll  = 0.0f;
    if (usedActorRotation) {
        yaw   = actorYaw;
        pitch = actorPitch;
    } else if (validDirection(right)) {
        right = normalized(right);
        ::glm::vec3 noRollRight;
        ::glm::vec3 noRollUp;
        cameraVectors(yaw, pitch, noRollRight, noRollUp);
        roll = std::atan2(dot(right, noRollUp), dot(right, noRollRight)) * DegreesPerRadian;
    }

    return ClientCameraCapture{
        {basePosition.x, basePosition.y, basePosition.z, yaw, pitch, roll},
        {cameraPosition.x, cameraPosition.y, cameraPosition.z},
        {nativeForward.x, nativeForward.y, nativeForward.z},
        {nativeRight.x, nativeRight.y, nativeRight.z},
        {nativeUp.x, nativeUp.y, nativeUp.z},
        {actorPitch, actorYaw, actorHeadYaw},
        {actorForward.x, actorForward.y, actorForward.z},
        usedNativeRotation,
        usedMatrixRotation,
        usedActorRotation,
        usedWorldCamera,
        usedWorldPosition,
        actor && actor == cameraActor,
        actor && actor == replayPlayer,
    };
}

} // namespace playback::editor::keyframe
