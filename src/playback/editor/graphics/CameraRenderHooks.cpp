#include "CameraRenderHooks.h"

#include "playback/Playback.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/minecraft_camera/CameraRegistry.h"
#include "mc/deps/minecraft_camera/components/ActiveCameraComponent.h"
#include "mc/deps/minecraft_camera/components/CameraComponent.h"
#include "mc/deps/minecraft_camera/components/RenderCameraComponent.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/world/actor/player/Player.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace playback::editor::graphics {

namespace {

constexpr float PositionTolerance  = 0.02f;
constexpr float DirectionTolerance = 0.002f;
constexpr auto  UnloggedFrame      = std::numeric_limits<uint64_t>::max();
constexpr float RadiansPerDegree   = std::numbers::pi_v<float> / 180.0f;

std::atomic_bool                     gInstalled{};
std::atomic<uint64_t>                gPreviewFrameSerial{};
std::array<std::atomic_bool, 2>      gMissingSampleLogged{};
std::array<std::atomic_bool, 2>      gApplicationFailureLogged{};
std::array<std::atomic_bool, 2>      gFinalViewFailureLogged{};
std::array<std::atomic_bool, 2>      gCameraEcsFailureLogged{};
std::array<std::atomic<uint64_t>, 2> gLastFinalViewFrame{};
std::array<std::atomic<uint64_t>, 2> gLastCameraEcsFrame{};

thread_local std::optional<keyframe::CameraRenderState> gParkedObserverCamera;

size_t sourceIndex(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? 1U : 0U;
}

char const* sourceName(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? "export" : "preview";
}

bool finite(keyframe::CameraRenderState const& state) noexcept {
    return std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.z) && std::isfinite(state.yaw)
        && std::isfinite(state.pitch) && std::isfinite(state.roll) && std::isfinite(state.fov) && state.fov > 1.0f
        && state.fov < 179.0f;
}

bool finite(::glm::vec3 const& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float dot(::glm::vec3 const& left, ::glm::vec3 const& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

::glm::vec3 normalized(::glm::vec3 value) noexcept {
    float const length = std::sqrt(dot(value, value));
    if (!std::isfinite(length) || length <= 0.0001f) return {};
    return value / length;
}

float maxError(::glm::vec3 const& actual, ::glm::vec3 const& expected) noexcept {
    if (!finite(actual) || !finite(expected)) return std::numeric_limits<float>::infinity();
    return std::max({
        std::abs(actual.x - expected.x),
        std::abs(actual.y - expected.y),
        std::abs(actual.z - expected.z),
    });
}

float maxError(Vec3 const& actual, ::glm::vec3 const& expected) noexcept {
    return maxError(::glm::vec3{actual.x, actual.y, actual.z}, expected);
}

struct CameraBasis {
    ::glm::vec3 right{};
    ::glm::vec3 up{};
    ::glm::vec3 forward{};
};

CameraBasis basisFromState(keyframe::CameraRenderState const& state) noexcept {
    float const yawRadians   = state.yaw * RadiansPerDegree;
    float const pitchRadians = state.pitch * RadiansPerDegree;
    float const sinYaw       = std::sin(yawRadians);
    float const cosYaw       = std::cos(yawRadians);
    float const sinPitch     = std::sin(pitchRadians);
    float const cosPitch     = std::cos(pitchRadians);

    CameraBasis result{
        {-cosYaw,            0.0f,      -sinYaw          },
        {-sinYaw * sinPitch, cosPitch,  cosYaw * sinPitch},
        {-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch},
    };

    float const rollRadians = state.roll * RadiansPerDegree;
    if (std::abs(rollRadians) > std::numeric_limits<float>::epsilon()) {
        float const cosine = std::cos(rollRadians);
        float const sine   = std::sin(rollRadians);
        auto const  right  = result.right;
        auto const  up     = result.up;
        result.right       = right * cosine + up * sine;
        result.up          = up * cosine - right * sine;
    }

    result.right   = normalized(result.right);
    result.up      = normalized(result.up);
    result.forward = normalized(result.forward);
    return result;
}

::glm::vec3 inverseViewPosition(mce::Camera const& camera) noexcept {
    auto const& inverse = camera.mInverseViewMatrix.get();
    return {inverse[3].x, inverse[3].y, inverse[3].z};
}

::glm::vec3 inverseViewForward(mce::Camera const& camera) noexcept {
    auto const& inverse = camera.mInverseViewMatrix.get();
    return normalized({-inverse[2].x, -inverse[2].y, -inverse[2].z});
}

struct CameraVerification {
    float publicPositionError{std::numeric_limits<float>::infinity()};
    float publicDirectionError{std::numeric_limits<float>::infinity()};
    float viewPositionError{std::numeric_limits<float>::infinity()};
    float viewDirectionError{std::numeric_limits<float>::infinity()};

    [[nodiscard]] bool valid() const noexcept {
        return publicPositionError <= PositionTolerance && publicDirectionError <= DirectionTolerance
            && viewPositionError <= PositionTolerance && viewDirectionError <= DirectionTolerance;
    }
};

CameraVerification
verifyCamera(mce::Camera const& camera, ::glm::vec3 const& position, CameraBasis const& basis) noexcept {
    return {
        maxError(camera.mPosition.get(), position),
        maxError(normalized(camera.mForward.get()), basis.forward),
        maxError(inverseViewPosition(camera), position),
        maxError(inverseViewForward(camera), basis.forward),
    };
}

::glm::mat4x4 viewMatrix(::glm::vec3 const& position, CameraBasis const& basis) noexcept {
    auto result = ::glm::mat4x4{1.0f};
    result[0]   = {basis.right.x, basis.up.x, -basis.forward.x, 0.0f};
    result[1]   = {basis.right.y, basis.up.y, -basis.forward.y, 0.0f};
    result[2]   = {basis.right.z, basis.up.z, -basis.forward.z, 0.0f};
    result[3]   = {
        -dot(basis.right, position),
        -dot(basis.up, position),
        dot(basis.forward, position),
        1.0f,
    };
    return result;
}

void writeCameraPose(mce::Camera& camera, ::glm::vec3 const& position, CameraBasis const& basis) noexcept {
    float const nativeFov    = camera.mFov;
    float const nativeAspect = camera.mAspectRatio;

    camera.viewMatrixStack->getTop()._m = viewMatrix(position, basis);
    camera.viewMatrixStack->_isDirty    = true;
    camera.updateViewMatrixDependencies();

    camera.mPosition    = position;
    camera.mRight       = basis.right;
    camera.mUp          = basis.up;
    camera.mForward     = basis.forward;
    camera.mFov         = nativeFov;
    camera.mAspectRatio = nativeAspect;
}

void writeLocalCameraPose(mce::Camera& camera, CameraBasis const& basis) noexcept {
    float const nativeFov    = camera.mFov;
    float const nativeAspect = camera.mAspectRatio;

    camera.viewMatrixStack->getTop()._m = viewMatrix({}, basis);
    camera.viewMatrixStack->_isDirty    = true;
    camera.updateViewMatrixDependencies();

    camera.mPosition    = ::glm::vec3{};
    camera.mRight       = basis.right;
    camera.mUp          = basis.up;
    camera.mForward     = basis.forward;
    camera.mFov         = nativeFov;
    camera.mAspectRatio = nativeAspect;
}

void writeCameraSpaces(
    std::array<mce::Camera*, 2> const& localCameras,
    mce::Camera&                       worldCamera,
    ::glm::vec3 const&                 position,
    CameraBasis const&                 basis
) noexcept {
    std::array<mce::Camera*, 2> applied{};
    size_t                      appliedCount{};
    for (auto* camera : localCameras) {
        if (!camera || camera == &worldCamera) continue;
        if (std::find(applied.begin(), applied.begin() + appliedCount, camera) != applied.begin() + appliedCount) {
            continue;
        }
        writeLocalCameraPose(*camera, basis);
        applied[appliedCount++] = camera;
    }
    writeCameraPose(worldCamera, position, basis);
}

void writeLevelCameraPose(LevelRendererPlayer& level, ::glm::vec3 const& position, CameraBasis const& basis) noexcept {
    level.mCameraPos       = Vec3{position.x, position.y, position.z};
    level.mCameraTargetPos = Vec3{
        position.x + basis.forward.x,
        position.y + basis.forward.y,
        position.z + basis.forward.z,
    };
    level.mCameraForward = Vec3{basis.forward.x, basis.forward.y, basis.forward.z};
    level.mCameraUp      = Vec3{basis.up.x, basis.up.y, basis.up.z};
}

bool shouldLogFinalView(keyframe::CameraTimelineRenderContext const& context) noexcept {
    bool const selected = context.source == keyframe::CameraTimelineSource::Export
                            ? context.frameIndex == 0 || context.frameIndex == 1 || context.frameIndex % 60U == 0
                            : context.frameIndex <= 1;
    if (!selected) return false;

    auto&      last     = gLastFinalViewFrame[sourceIndex(context.source)];
    auto const previous = last.exchange(context.frameIndex, std::memory_order_acq_rel);
    return previous != context.frameIndex;
}

bool shouldLogCameraEcs(keyframe::CameraTimelineRenderContext const& context) noexcept {
    bool const selected = context.source == keyframe::CameraTimelineSource::Export
                            ? context.frameIndex == 0 || context.frameIndex == 1 || context.frameIndex % 60U == 0
                            : context.frameIndex <= 3 || context.frameIndex % 60U == 0;
    if (!selected) return false;

    auto&      last     = gLastCameraEcsFrame[sourceIndex(context.source)];
    auto const previous = last.exchange(context.frameIndex, std::memory_order_acq_rel);
    return previous != context.frameIndex;
}

void logMissingSample(keyframe::CameraTimelineRenderContext const& context) noexcept {
    auto& logged = gMissingSampleLogged[sourceIndex(context.source)];
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    Playback::getInstance().getSelf().getLogger().warn(
        "Camera render context has no sample (source={}, token={}, frame={}, tick={}/{})",
        sourceName(context.source),
        context.renderToken,
        context.frameIndex,
        context.time.numerator,
        context.time.denominator
    );
}

::glm::qua<float> orientationFromBasis(CameraBasis const& basis) noexcept {
    ::glm::mat3 const cameraToWorld{basis.right, basis.up, -basis.forward};
    return ::glm::normalize(::glm::quat_cast(cameraToWorld));
}

bool applyCameraEcs(keyframe::CameraTimelineRenderContext const& context) noexcept {
    if (!context.sample || !finite(context.sample->state)) return false;

    auto client   = ll::service::getClientInstance();
    auto registry = client ? client->getCameraRegistry() : Bedrock::NonOwnerPointer<CameraRegistry>{};
    if (!registry) return false;

    auto const& state        = context.sample->state;
    auto const  position     = ::glm::vec3{state.x, state.y, state.z};
    auto const  basis        = basisFromState(state);
    auto const  modelView    = viewMatrix(position, basis);
    auto const  componentFov = state.fov * RadiansPerDegree;

    struct AppliedComponent {
        MinecraftCamera::CameraComponent* component{};
        ::glm::qua<float>                 orientation{};
    };
    std::vector<AppliedComponent> components;
    components.reserve(registry->mCameraEntities->size() + 1);

    ::glm::vec3       beforePosition{};
    ::glm::qua<float> beforeOrientation{};
    float             beforeFov{};
    bool              capturedBefore{};
    bool              hasGameCamera{};

    auto applyEntity = [&](EntityContext& entity, bool gameCamera) {
        auto* component = entity.tryGetComponent<MinecraftCamera::CameraComponent>().as_ptr();
        if (!component) return;
        if (!gameCamera && !entity.hasComponent<MinecraftCamera::ActiveCameraComponent>()
            && !entity.hasComponent<MinecraftCamera::RenderCameraComponent>()) {
            return;
        }
        if (std::ranges::find(components, component, &AppliedComponent::component) != components.end()) return;

        if (!capturedBefore) {
            beforePosition    = component->mPosition.get();
            beforeOrientation = component->mOrientation.get();
            beforeFov         = component->mFieldOfView;
            capturedBefore    = true;
        }
        auto const orientation         = orientationFromBasis(basis);
        component->mPosition           = position;
        component->mOrientation        = orientation;
        component->mFieldOfView        = componentFov;
        component->mSavedModelView->_m = modelView;
        components.push_back({component, orientation});
        hasGameCamera = hasGameCamera || gameCamera;
    };

    auto& gameCamera = registry->mGameCamera.get();
    if (gameCamera) applyEntity(*gameCamera, true);
    for (auto& cameraEntity : registry->mCameraEntities.get()) {
        if (cameraEntity) applyEntity(*cameraEntity, false);
    }

    bool verified = !components.empty();
    for (auto const& applied : components) {
        auto const& appliedOrientation = applied.component->mOrientation.get();
        float const orientationDot     = std::abs(::glm::dot(appliedOrientation, applied.orientation));
        verified = verified && maxError(applied.component->mPosition.get(), position) <= PositionTolerance
                && std::abs(applied.component->mFieldOfView - componentFov) <= 0.001f && std::isfinite(orientationDot)
                && orientationDot >= 0.9999f;
    }

    bool const logSelected = shouldLogCameraEcs(context);
    auto&      failureFlag = gCameraEcsFailureLogged[sourceIndex(context.source)];
    bool const logFailure  = !verified && !failureFlag.exchange(true, std::memory_order_acq_rel);
    if (logSelected || logFailure) {
        auto const* applied          = components.empty() ? nullptr : components.front().component;
        auto const  afterPosition    = applied ? applied->mPosition.get() : ::glm::vec3{};
        auto const  afterOrientation = applied ? applied->mOrientation.get() : ::glm::qua<float>{};
        float const afterFov         = applied ? applied->mFieldOfView : 0.0f;
        Playback::getInstance().getSelf().getLogger().debug(
            "Camera ECS apply (source={}, token={}, frame={}, tick={}/{}, cameraId={}, targets={}, gameCamera={}, "
            "sample=(position=({}, {}, {}), yaw={}, pitch={}, roll={}, fov={}), "
            "before=(position=({}, {}, {}), orientation=({}, {}, {}, {}), fov={}), "
            "after=(position=({}, {}, {}), orientation=({}, {}, {}, {}), fov={}), verified={})",
            sourceName(context.source),
            context.renderToken,
            context.frameIndex,
            context.time.numerator,
            context.time.denominator,
            context.sample->cameraId,
            components.size(),
            hasGameCamera,
            state.x,
            state.y,
            state.z,
            state.yaw,
            state.pitch,
            state.roll,
            state.fov,
            beforePosition.x,
            beforePosition.y,
            beforePosition.z,
            beforeOrientation.w,
            beforeOrientation.x,
            beforeOrientation.y,
            beforeOrientation.z,
            beforeFov,
            afterPosition.x,
            afterPosition.y,
            afterPosition.z,
            afterOrientation.w,
            afterOrientation.x,
            afterOrientation.y,
            afterOrientation.z,
            afterFov,
            verified
        );
    }
    return verified;
}

bool applyTimelineCamera(
    LevelRendererPlayer&                         level,
    mce::Camera&                                 setupCamera,
    keyframe::CameraTimelineRenderContext const& context
) noexcept {
    if (!context.sample) {
        logMissingSample(context);
        return false;
    }

    auto const& sample = *context.sample;
    if (!finite(sample.state)) return false;

    auto const position = ::glm::vec3{sample.state.x, sample.state.y, sample.state.z};
    auto const basis    = basisFromState(sample.state);
    if (!finite(position) || !finite(basis.right) || !finite(basis.up) || !finite(basis.forward)) return false;

    auto  client       = ll::service::getClientInstance();
    auto* clientCamera = client ? &client->getCamera() : nullptr;
    auto* worldCamera  = &level.mWorldSpaceCamera.get();

    writeCameraSpaces({&setupCamera, clientCamera}, *worldCamera, position, basis);
    writeLevelCameraPose(level, position, basis);

    auto const localPosition  = ::glm::vec3{};
    auto const setupExpected  = &setupCamera == worldCamera ? position : localPosition;
    auto const clientExpected = clientCamera == worldCamera ? position : localPosition;
    auto const setupCheck     = verifyCamera(setupCamera, setupExpected, basis);
    auto const clientCheck = clientCamera ? verifyCamera(*clientCamera, clientExpected, basis) : CameraVerification{};
    auto const worldCheck  = verifyCamera(*worldCamera, position, basis);

    float const levelPositionError  = maxError(level.mCameraPos.get(), position);
    float const levelDirectionError = maxError(
        normalized({
            level.mCameraTargetPos->x - level.mCameraPos->x,
            level.mCameraTargetPos->y - level.mCameraPos->y,
            level.mCameraTargetPos->z - level.mCameraPos->z,
        }),
        basis.forward
    );
    bool const verified = setupCheck.valid() && (!clientCamera || clientCheck.valid()) && worldCheck.valid()
                       && levelPositionError <= PositionTolerance && levelDirectionError <= DirectionTolerance;

    auto&      failureFlag = gApplicationFailureLogged[sourceIndex(context.source)];
    bool const logFailure  = !verified && !failureFlag.exchange(true, std::memory_order_acq_rel);
    if (logFailure) {
        Playback::getInstance().getSelf().getLogger().error(
            "Camera sample did not reach the renderer-owned world camera (source={}, token={}, frame={}, "
            "worldErrors=({}, {}, {}, {}), levelErrors=({}, {}))",
            sourceName(context.source),
            context.renderToken,
            context.frameIndex,
            worldCheck.publicPositionError,
            worldCheck.viewPositionError,
            worldCheck.publicDirectionError,
            worldCheck.viewDirectionError,
            levelPositionError,
            levelDirectionError
        );
    }
    return verified;
}

void applyObserverCamera(
    LevelRendererPlayer&               level,
    mce::Camera&                       setupCamera,
    keyframe::CameraRenderState const& state
) noexcept {
    if (!finite(state)) return;
    auto const position = ::glm::vec3{state.x, state.y, state.z};
    auto const basis    = basisFromState(state);
    if (!finite(position) || !finite(basis.right) || !finite(basis.up) || !finite(basis.forward)) return;

    auto  client       = ll::service::getClientInstance();
    auto* clientCamera = client ? &client->getCamera() : nullptr;
    auto* worldCamera  = &level.mWorldSpaceCamera.get();
    writeCameraSpaces({&setupCamera, clientCamera}, *worldCamera, position, basis);
    writeLevelCameraPose(level, position, basis);
}

bool observerStillParked(keyframe::CameraRenderState const& state) noexcept {
    auto* observer = replay::ReplaySession::getInstance().getReplayPlayer();
    if (!observer) return false;

    auto const position      = observer->getPosition();
    auto const rotation      = observer->getRotation();
    auto const positionDelta = ::glm::vec3{
        position.x - state.x,
        position.y - state.y,
        position.z - state.z,
    };
    auto const angleDelta = [](float left, float right) { return std::abs(std::remainder(left - right, 360.0f)); };
    return dot(positionDelta, positionDelta) <= 0.05f * 0.05f && angleDelta(rotation.x, state.pitch) <= 0.1f
        && angleDelta(rotation.y, state.yaw) <= 0.1f;
}

keyframe::CameraTimelineRenderContextHandle makePreviewRenderContext(float partialTick) noexcept {
    auto& replay = replay::ReplaySession::getInstance();

    auto const clear = [] { keyframe::setPreviewCameraApplied(false); };

    if (!keyframe::hasCameraTimeline(keyframe::CameraTimelineSource::Preview)) {
        clear();
        return {};
    }
    if (replay.isPaused()) {
        clear();
        return {};
    }

    auto const time = replay.getCameraRenderSampleTime(partialTick);
    if (!time) {
        clear();
        return {};
    }
    auto sample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *time);
    if (!sample) {
        clear();
        return {};
    }

    auto const serial = gPreviewFrameSerial.fetch_add(1, std::memory_order_relaxed) + 1;
    keyframe::setPreviewCameraApplied(true);
    keyframe::setLastPreviewPose(sample->state);
    gParkedObserverCamera = sample->state;
    return keyframe::publishCameraTimelineRenderContext(
        keyframe::CameraTimelineRenderContext{
            *time,
            keyframe::CameraTimelineSource::Preview,
            0,
            std::move(sample),
            {},
            serial,
        }
    );
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraFrameScopeHook,
    ll::memory::HookPriority::Lowest,
    GameRenderer,
    &GameRenderer::renderCurrentFrame,
    void,
    float partialTick
) {
    replay::ReplaySession::getInstance().setObserverPreviewPartialTick(partialTick);

    auto const existing = keyframe::currentCameraTimelineRenderContext();
    if (existing && existing->source == keyframe::CameraTimelineSource::Export) {
        (void)applyCameraEcs(*existing);
        origin(partialTick);
        return;
    }

    auto const context = makePreviewRenderContext(partialTick);
    if (!context) {
        keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview);
        if (!keyframe::hasCameraTimeline(keyframe::CameraTimelineSource::Preview)) gParkedObserverCamera.reset();
        origin(partialTick);
        return;
    }

    {
        keyframe::ScopedCameraTimelineRenderContext scope(context);
        origin(partialTick);
    }
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview, context);
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraFovHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::getFov,
    float,
    float amount,
    bool  enableVariableFov
) {
    float const native  = origin(amount, enableVariableFov);
    auto const  context = keyframe::currentCameraTimelineRenderContext();
    if (context && context->sample && finite(context->sample->state)) return context->sample->state.fov;
    return gParkedObserverCamera && observerStillParked(*gParkedObserverCamera) ? gParkedObserverCamera->fov : native;
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraFovWithoutGameplayHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::getFovWithoutGameplay,
    float
) {
    float const native  = origin();
    auto const  context = keyframe::currentCameraTimelineRenderContext();
    if (context && context->sample && finite(context->sample->state)) return context->sample->state.fov;
    return gParkedObserverCamera && observerStillParked(*gParkedObserverCamera) ? gParkedObserverCamera->fov : native;
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraSetupHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,
    void,
    mce::Camera& camera,
    float        partialTick
) {
    origin(camera, partialTick);

    auto const context = keyframe::currentCameraTimelineRenderContext();
    if (context) {
        // Preview/export render the camera exactly from the timeline pose (smooth; no per-frame
        // observer teleport round-trip).
        if (context->source == keyframe::CameraTimelineSource::Export) {
            (void)applyCameraEcs(*context);
        }
        (void)applyTimelineCamera(*static_cast<LevelRendererPlayer*>(this), camera, *context);
        return;
    }

    if (gParkedObserverCamera) {
        if (observerStillParked(*gParkedObserverCamera)) {
            applyObserverCamera(*static_cast<LevelRendererPlayer*>(this), camera, *gParkedObserverCamera);
        } else {
            gParkedObserverCamera.reset();
        }
    }
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraViewRenderObjectHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::createViewRenderObject,
    ::ViewRenderObject,
    ::ScreenContext& screenContext,
    ::SubClientId    clientSubId
) {
    auto const context = keyframe::currentCameraTimelineRenderContext();
    if (!context || context->source != keyframe::CameraTimelineSource::Export)
        return origin(screenContext, clientSubId);
    if (!context->sample) {
        logMissingSample(*context);
        return origin(screenContext, clientSubId);
    }

    auto const& sample = *context->sample;
    if (!finite(sample.state)) return origin(screenContext, clientSubId);

    auto const position = ::glm::vec3{sample.state.x, sample.state.y, sample.state.z};
    auto const basis    = basisFromState(sample.state);
    if (!finite(position) || !finite(basis.right) || !finite(basis.up) || !finite(basis.forward)) {
        return origin(screenContext, clientSubId);
    }

    auto& level        = *static_cast<LevelRendererPlayer*>(this);
    auto& worldCamera  = level.mWorldSpaceCamera.get();
    auto  client       = ll::service::getClientInstance();
    auto* clientCamera = client ? &client->getCamera() : nullptr;

    (void)applyCameraEcs(*context);
    writeCameraSpaces({clientCamera, nullptr}, worldCamera, position, basis);
    writeLevelCameraPose(level, position, basis);

    auto       renderObject       = origin(screenContext, clientSubId);
    auto&      view               = renderObject.mViewData.get();
    auto const nativeViewPosition = view.mCameraPos.get();
    auto const nativeViewTarget   = view.mCameraTargetPos.get();

    (void)applyCameraEcs(*context);
    writeCameraSpaces({clientCamera, nullptr}, worldCamera, position, basis);
    writeLevelCameraPose(level, position, basis);
    view.mCameraPos       = position;
    view.mCameraTargetPos = position + basis.forward;

    auto const  worldCheck          = verifyCamera(worldCamera, position, basis);
    float const levelPositionError  = maxError(level.mCameraPos.get(), position);
    float const levelDirectionError = maxError(
        normalized({
            level.mCameraTargetPos->x - level.mCameraPos->x,
            level.mCameraTargetPos->y - level.mCameraPos->y,
            level.mCameraTargetPos->z - level.mCameraPos->z,
        }),
        basis.forward
    );
    float const viewPositionError = maxError(view.mCameraPos.get(), position);
    float const viewDirectionError =
        maxError(normalized(view.mCameraTargetPos.get() - view.mCameraPos.get()), basis.forward);
    bool const verified = worldCheck.valid() && levelPositionError <= PositionTolerance
                       && levelDirectionError <= DirectionTolerance && viewPositionError <= PositionTolerance
                       && viewDirectionError <= DirectionTolerance;

    if (verified && context->appliedFlag) context->appliedFlag->store(true, std::memory_order_release);

    bool const logSelected = shouldLogFinalView(*context);
    auto&      failureFlag = gFinalViewFailureLogged[sourceIndex(context->source)];
    bool const logFailure  = !verified && !failureFlag.exchange(true, std::memory_order_acq_rel);
    if (logSelected || logFailure) {
        Playback::getInstance().getSelf().getLogger().debug(
            "Camera final view (source={}, token={}, frame={}, tick={}/{}, cameraId={}, "
            "sample=(position=({}, {}, {}), yaw={}, pitch={}, roll={}, fov={}), "
            "nativeView=(position=({}, {}, {}), target=({}, {}, {})), "
            "final=(view=({}, {}, {}), target=({}, {}, {}), fovRadians={}), "
            "errors=(world=({}, {}, {}, {}), level=({}, {}), view=({}, {})), verified={})",
            sourceName(context->source),
            context->renderToken,
            context->frameIndex,
            context->time.numerator,
            context->time.denominator,
            sample.cameraId,
            position.x,
            position.y,
            position.z,
            sample.state.yaw,
            sample.state.pitch,
            sample.state.roll,
            sample.state.fov,
            nativeViewPosition.x,
            nativeViewPosition.y,
            nativeViewPosition.z,
            nativeViewTarget.x,
            nativeViewTarget.y,
            nativeViewTarget.z,
            view.mCameraPos->x,
            view.mCameraPos->y,
            view.mCameraPos->z,
            view.mCameraTargetPos->x,
            view.mCameraTargetPos->y,
            view.mCameraTargetPos->z,
            worldCamera.mFov,
            worldCheck.publicPositionError,
            worldCheck.viewPositionError,
            worldCheck.publicDirectionError,
            worldCheck.viewDirectionError,
            levelPositionError,
            levelDirectionError,
            viewPositionError,
            viewDirectionError,
            verified
        );
    }
    if (logFailure) {
        Playback::getInstance().getSelf().getLogger().error(
            "Camera sample did not reach the final ViewRenderObject (source={}, token={}, frame={})",
            sourceName(context->source),
            context->renderToken,
            context->frameIndex
        );
    }
    return renderObject;
}

} // namespace

bool hookCameraRender(bool enable) {
    struct HookState {
        bool frameScope{};
        bool fov{};
        bool fovWithoutGameplay{};
        bool setupCamera{};
        bool finalView{};
    };
    static HookState state;

    auto removeAll = [&] {
        gInstalled.store(false, std::memory_order_release);
        gParkedObserverCamera.reset();
        keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview);
        if (state.finalView && ReplayCameraViewRenderObjectHook::unhook()) state.finalView = false;
        if (state.setupCamera && ReplayCameraSetupHook::unhook()) state.setupCamera = false;
        if (state.fovWithoutGameplay && ReplayCameraFovWithoutGameplayHook::unhook()) {
            state.fovWithoutGameplay = false;
        }
        if (state.fov && ReplayCameraFovHook::unhook()) state.fov = false;
        if (state.frameScope && ReplayCameraFrameScopeHook::unhook()) state.frameScope = false;
        return !state.frameScope && !state.fov && !state.fovWithoutGameplay && !state.setupCamera && !state.finalView;
    };

    if (!enable) return removeAll();

    for (auto& flag : gMissingSampleLogged) flag.store(false, std::memory_order_release);
    for (auto& flag : gApplicationFailureLogged) flag.store(false, std::memory_order_release);
    for (auto& flag : gFinalViewFailureLogged) flag.store(false, std::memory_order_release);
    for (auto& flag : gCameraEcsFailureLogged) flag.store(false, std::memory_order_release);
    for (auto& frame : gLastFinalViewFrame) frame.store(UnloggedFrame, std::memory_order_release);
    for (auto& frame : gLastCameraEcsFrame) frame.store(UnloggedFrame, std::memory_order_release);
    gPreviewFrameSerial.store(0, std::memory_order_release);
    gParkedObserverCamera.reset();
    keyframe::setPreviewCameraApplied(false);
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview);

    if (!state.frameScope) state.frameScope = ReplayCameraFrameScopeHook::hook() == 0;
    if (!state.fov) state.fov = ReplayCameraFovHook::hook() == 0;
    if (!state.fovWithoutGameplay) {
        state.fovWithoutGameplay = ReplayCameraFovWithoutGameplayHook::hook() == 0;
    }
    if (!state.setupCamera) state.setupCamera = ReplayCameraSetupHook::hook() == 0;
    if (!state.finalView) state.finalView = ReplayCameraViewRenderObjectHook::hook() == 0;

    bool const installed =
        state.frameScope && state.fov && state.fovWithoutGameplay && state.setupCamera && state.finalView;
    gInstalled.store(installed, std::memory_order_release);
    if (!installed) {
        bool const frameScope         = state.frameScope;
        bool const fov                = state.fov;
        bool const fovWithoutGameplay = state.fovWithoutGameplay;
        bool const setupCamera        = state.setupCamera;
        bool const finalView          = state.finalView;
        bool const rolledBack         = removeAll();
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install camera render hooks (frameScope={}, fov={}, fovWithoutGameplay={}, setupCamera={}, "
            "finalView={}, rollback={})",
            frameScope,
            fov,
            fovWithoutGameplay,
            setupCamera,
            finalView,
            rolledBack
        );
        return false;
    }
    Playback::getInstance().getSelf().getLogger().info(
        "Camera render hooks installed (frameScope=true, cameraEcs=true, fov=true, actorPose=false, "
        "setupCamera=true, finalViewHook=true)"
    );
    return true;
}

bool isCameraRenderInstalled() { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::editor::graphics
