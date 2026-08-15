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
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace playback::editor::host {

namespace {

constexpr float RadiansPerDegree   = 3.14159265358979323846f / 180.0f;
constexpr float PositionTolerance  = 0.02f;
constexpr float DirectionTolerance = 0.002f;
constexpr auto  UnloggedFrame      = std::numeric_limits<uint64_t>::max();

std::atomic_bool                     gInstalled{};
std::atomic<uint64_t>                gPreviewFrameSerial{};
std::array<std::atomic_bool, 2>      gMissingSampleLogged{};
std::array<std::atomic_bool, 2>      gApplicationFailureLogged{};
std::array<std::atomic_bool, 2>      gFinalViewFailureLogged{};
std::array<std::atomic<uint64_t>, 2> gLastAppliedFrame{};
std::array<std::atomic<uint64_t>, 2> gLastFinalViewFrame{};

size_t sourceIndex(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? 1U : 0U;
}

char const* sourceName(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? "export" : "preview";
}

bool finite(keyframe::CameraRenderState const& state) noexcept {
    return std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.z) && std::isfinite(state.yaw)
        && std::isfinite(state.pitch) && std::isfinite(state.roll);
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

struct CameraSnapshot {
    ::glm::vec3 position{};
    ::glm::vec3 forward{};
    ::glm::vec3 viewPosition{};
    ::glm::vec3 viewForward{};
    float       fov{};
    float       aspect{};
};

CameraSnapshot snapshot(mce::Camera const& camera) noexcept {
    return {
        camera.mPosition.get(),
        camera.mForward.get(),
        inverseViewPosition(camera),
        inverseViewForward(camera),
        camera.mFov,
        camera.mAspectRatio,
    };
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

void writeCameraPose(mce::Camera& camera, ::glm::vec3 const& position, CameraBasis const& basis) noexcept {
    float const nativeFov    = camera.mFov;
    float const nativeAspect = camera.mAspectRatio;

    auto& view = camera.viewMatrixStack->getTop()._m.get();
    view       = ::glm::mat4x4{1.0f};
    view[0]    = {basis.right.x, basis.up.x, -basis.forward.x, 0.0f};
    view[1]    = {basis.right.y, basis.up.y, -basis.forward.y, 0.0f};
    view[2]    = {basis.right.z, basis.up.z, -basis.forward.z, 0.0f};
    view[3]    = {
        -dot(basis.right, position),
        -dot(basis.up, position),
        dot(basis.forward, position),
        1.0f,
    };
    camera.viewMatrixStack->_isDirty = true;
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

    auto& view                       = camera.viewMatrixStack->getTop()._m.get();
    view                             = ::glm::mat4x4{1.0f};
    view[0]                          = {basis.right.x, basis.up.x, -basis.forward.x, 0.0f};
    view[1]                          = {basis.right.y, basis.up.y, -basis.forward.y, 0.0f};
    view[2]                          = {basis.right.z, basis.up.z, -basis.forward.z, 0.0f};
    camera.viewMatrixStack->_isDirty = true;
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

bool shouldLogApplied(keyframe::CameraTimelineRenderContext const& context) noexcept {
    bool const selected = context.source == keyframe::CameraTimelineSource::Export
                            ? context.frameIndex == 0 || context.frameIndex == 1 || context.frameIndex % 60U == 0
                            : context.frameIndex <= 1;
    if (!selected) return false;

    auto&      last     = gLastAppliedFrame[sourceIndex(context.source)];
    auto const previous = last.exchange(context.frameIndex, std::memory_order_acq_rel);
    return previous != context.frameIndex;
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

    auto const setupBefore  = snapshot(setupCamera);
    auto const clientBefore = clientCamera ? snapshot(*clientCamera) : CameraSnapshot{};
    auto const worldBefore  = snapshot(*worldCamera);

    writeCameraSpaces({&setupCamera, clientCamera}, *worldCamera, position, basis);
    writeLevelCameraPose(level, position, basis);

    auto const setupAfter     = snapshot(setupCamera);
    auto const clientAfter    = clientCamera ? snapshot(*clientCamera) : CameraSnapshot{};
    auto const worldAfter     = snapshot(*worldCamera);
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

    bool const logSelected = shouldLogApplied(context);
    auto&      failureFlag = gApplicationFailureLogged[sourceIndex(context.source)];
    bool const logFailure  = !verified && !failureFlag.exchange(true, std::memory_order_acq_rel);
    if (logSelected || logFailure) {
        auto& logger = Playback::getInstance().getSelf().getLogger();
        logger.info(
            "Camera sample at setupCamera (source={}, token={}, frame={}, tick={}/{}, cameraId={}, "
            "sample=({}, {}, {}), rotation=({}, {}, {}), aliases=(setupClient={}, setupWorld={}, clientWorld={}), "
            "nativeSetup=({}, {}, {}, forward=({}, {}, {}), fov={}, aspect={}), verified={})",
            sourceName(context.source),
            context.renderToken,
            context.frameIndex,
            context.time.numerator,
            context.time.denominator,
            sample.cameraId,
            position.x,
            position.y,
            position.z,
            sample.state.yaw,
            sample.state.pitch,
            sample.state.roll,
            clientCamera == &setupCamera,
            worldCamera == &setupCamera,
            clientCamera && clientCamera == worldCamera,
            setupBefore.position.x,
            setupBefore.position.y,
            setupBefore.position.z,
            setupBefore.forward.x,
            setupBefore.forward.y,
            setupBefore.forward.z,
            setupBefore.fov,
            setupBefore.aspect,
            verified
        );
        logger.info(
            "Camera target verification (source={}, frame={}, "
            "setup=(position=({}, {}, {}), view=({}, {}, {}), forward=({}, {}, {}), errors=({}, {}, {}, {})), "
            "client=(position=({}, {}, {}), view=({}, {}, {}), forward=({}, {}, {}), errors=({}, {}, {}, {})), "
            "world=(before=({}, {}, {}), position=({}, {}, {}), view=({}, {}, {}), forward=({}, {}, {}), "
            "errors=({}, {}, {}, {})), level=({}, {}, {}, errors=({}, {})), "
            "projectionPreserved=(setup={}, client={}, world={}))",
            sourceName(context.source),
            context.frameIndex,
            setupAfter.position.x,
            setupAfter.position.y,
            setupAfter.position.z,
            setupAfter.viewPosition.x,
            setupAfter.viewPosition.y,
            setupAfter.viewPosition.z,
            setupAfter.forward.x,
            setupAfter.forward.y,
            setupAfter.forward.z,
            setupCheck.publicPositionError,
            setupCheck.viewPositionError,
            setupCheck.publicDirectionError,
            setupCheck.viewDirectionError,
            clientAfter.position.x,
            clientAfter.position.y,
            clientAfter.position.z,
            clientAfter.viewPosition.x,
            clientAfter.viewPosition.y,
            clientAfter.viewPosition.z,
            clientAfter.forward.x,
            clientAfter.forward.y,
            clientAfter.forward.z,
            clientCheck.publicPositionError,
            clientCheck.viewPositionError,
            clientCheck.publicDirectionError,
            clientCheck.viewDirectionError,
            worldBefore.position.x,
            worldBefore.position.y,
            worldBefore.position.z,
            worldAfter.position.x,
            worldAfter.position.y,
            worldAfter.position.z,
            worldAfter.viewPosition.x,
            worldAfter.viewPosition.y,
            worldAfter.viewPosition.z,
            worldAfter.forward.x,
            worldAfter.forward.y,
            worldAfter.forward.z,
            worldCheck.publicPositionError,
            worldCheck.viewPositionError,
            worldCheck.publicDirectionError,
            worldCheck.viewDirectionError,
            level.mCameraPos->x,
            level.mCameraPos->y,
            level.mCameraPos->z,
            levelPositionError,
            levelDirectionError,
            setupAfter.fov == setupBefore.fov && setupAfter.aspect == setupBefore.aspect,
            !clientCamera || (clientAfter.fov == clientBefore.fov && clientAfter.aspect == clientBefore.aspect),
            worldAfter.fov == worldBefore.fov && worldAfter.aspect == worldBefore.aspect
        );
    }
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

keyframe::CameraTimelineRenderContextHandle makePreviewRenderContext(float partialTick) noexcept {
    auto& replay = replay::ReplaySession::getInstance();
    if (replay.isPaused()) return {};
    if (!keyframe::hasCameraTimeline(keyframe::CameraTimelineSource::Preview)) return {};

    auto const time = replay.getCameraRenderSampleTime(partialTick);
    if (!time) return {};
    auto sample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *time);
    if (!sample) return {};

    auto const serial = gPreviewFrameSerial.fetch_add(1, std::memory_order_relaxed) + 1;
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
    auto const existing = keyframe::currentCameraTimelineRenderContext();
    if (existing && existing->source == keyframe::CameraTimelineSource::Export) {
        origin(partialTick);
        return;
    }

    auto const context = makePreviewRenderContext(partialTick);
    if (!context) {
        keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview);
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
    if (!context) return;
    (void)applyTimelineCamera(*static_cast<LevelRendererPlayer*>(this), camera, *context);
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
    if (!context) return origin(screenContext, clientSubId);
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

    auto const worldBefore = snapshot(worldCamera);
    Vec3 const levelBefore = level.mCameraPos.get();

    writeCameraSpaces({clientCamera, nullptr}, worldCamera, position, basis);
    writeLevelCameraPose(level, position, basis);

    auto       renderObject       = origin(screenContext, clientSubId);
    auto&      view               = renderObject.mViewData.get();
    auto const nativeViewPosition = view.mCameraPos.get();
    auto const nativeViewTarget   = view.mCameraTargetPos.get();

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
        auto const worldAfter = snapshot(worldCamera);
        Playback::getInstance().getSelf().getLogger().info(
            "Camera sample at final view (source={}, token={}, frame={}, tick={}/{}, cameraId={}, "
            "sample=({}, {}, {}), rotation=({}, {}, {}), before=(world=({}, {}, {}), level=({}, {}, {})), "
            "nativeView=(position=({}, {}, {}), target=({}, {}, {})), "
            "final=(world=({}, {}, {}), view=({}, {}, {}), target=({}, {}, {})), "
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
            worldBefore.position.x,
            worldBefore.position.y,
            worldBefore.position.z,
            levelBefore.x,
            levelBefore.y,
            levelBefore.z,
            nativeViewPosition.x,
            nativeViewPosition.y,
            nativeViewPosition.z,
            nativeViewTarget.x,
            nativeViewTarget.y,
            nativeViewTarget.z,
            worldAfter.position.x,
            worldAfter.position.y,
            worldAfter.position.z,
            view.mCameraPos->x,
            view.mCameraPos->y,
            view.mCameraPos->z,
            view.mCameraTargetPos->x,
            view.mCameraTargetPos->y,
            view.mCameraTargetPos->z,
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
        bool setupCamera{};
        bool finalView{};
    };
    static HookState state;

    auto removeAll = [&] {
        gInstalled.store(false, std::memory_order_release);
        keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview);
        if (state.finalView && ReplayCameraViewRenderObjectHook::unhook()) state.finalView = false;
        if (state.setupCamera && ReplayCameraSetupHook::unhook()) state.setupCamera = false;
        if (state.frameScope && ReplayCameraFrameScopeHook::unhook()) state.frameScope = false;
        return !state.frameScope && !state.setupCamera && !state.finalView;
    };

    if (!enable) return removeAll();

    for (auto& flag : gMissingSampleLogged) flag.store(false, std::memory_order_release);
    for (auto& flag : gApplicationFailureLogged) flag.store(false, std::memory_order_release);
    for (auto& flag : gFinalViewFailureLogged) flag.store(false, std::memory_order_release);
    for (auto& frame : gLastAppliedFrame) frame.store(UnloggedFrame, std::memory_order_release);
    for (auto& frame : gLastFinalViewFrame) frame.store(UnloggedFrame, std::memory_order_release);
    gPreviewFrameSerial.store(0, std::memory_order_release);
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Preview);

    if (!state.frameScope) state.frameScope = ReplayCameraFrameScopeHook::hook() == 0;
    if (!state.setupCamera) state.setupCamera = ReplayCameraSetupHook::hook() == 0;
    if (!state.finalView) state.finalView = ReplayCameraViewRenderObjectHook::hook() == 0;

    bool const installed = state.frameScope && state.setupCamera && state.finalView;
    gInstalled.store(installed, std::memory_order_release);
    if (!installed) {
        bool const frameScope  = state.frameScope;
        bool const setupCamera = state.setupCamera;
        bool const finalView   = state.finalView;
        bool const rolledBack  = removeAll();
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install camera render hooks (frameScope={}, setupCamera={}, finalView={}, rollback={})",
            frameScope,
            setupCamera,
            finalView,
            rolledBack
        );
        return false;
    }
    Playback::getInstance().getSelf().getLogger().info(
        "Camera render hooks installed (frameScope=true, setupCamera=true, actorPose=false, finalViewHook=true)"
    );
    return true;
}

bool isCameraRenderInstalled() { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::editor::host
