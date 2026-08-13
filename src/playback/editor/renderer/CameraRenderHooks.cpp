#include "CameraRenderHooks.h"

#include "playback/Playback.h"
#include "playback/editor/keyframe/CameraTimelineRegistry.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/client/renderer/game/LevelRendererCamera.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace playback::editor::renderer {

namespace {

constexpr float RadiansPerDegree = 3.14159265358979323846f / 180.0f;

enum class CameraApplicationStage : size_t { FramePre, SetupFinal, ViewRenderObject, FramePost, Count };
constexpr size_t CameraApplicationStageCount = static_cast<size_t>(CameraApplicationStage::Count);
std::array<std::atomic_bool, 2> gMissingSampleLogged{};
std::array<std::array<std::atomic_bool, CameraApplicationStageCount>, 2> gDiagnosticLogged{};

size_t sourceIndex(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? 1U : 0U;
}

char const* sourceName(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? "export" : "preview";
}

float dot(::glm::vec3 const& left, ::glm::vec3 const& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

std::optional<keyframe::CameraTimelineSample> resolveSample(
    keyframe::CameraTimelineRenderContext const& context,
    bool&                                        operatorOverride
) noexcept;

void applyCameraState(mce::Camera& camera, keyframe::CameraTimelineSample const& sample) noexcept {
    auto const& state        = sample.state;
    float const yawRadians   = state.yaw * RadiansPerDegree;
    float const pitchRadians = state.pitch * RadiansPerDegree;
    float const cosPitch     = std::cos(pitchRadians);
    float const sinPitch     = std::sin(pitchRadians);
    float const sinYaw       = std::sin(yawRadians);
    float const cosYaw       = std::cos(yawRadians);

    ::glm::vec3 forward{-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch};
    ::glm::vec3 right{cosYaw, 0.0f, sinYaw};
    ::glm::vec3 up{-sinYaw * sinPitch, cosPitch, cosYaw * sinPitch};
    float const rollRadians = state.roll * RadiansPerDegree;
    if (std::abs(rollRadians) > std::numeric_limits<float>::epsilon()) {
        float const       cosine      = std::cos(rollRadians);
        float const       sine        = std::sin(rollRadians);
        ::glm::vec3 const rolledRight = right * cosine + up * sine;
        ::glm::vec3 const rolledUp    = up * cosine - right * sine;
        right                         = rolledRight;
        up                            = rolledUp;
    }

    auto const position   = ::glm::vec3{state.x, state.y, state.z};
    float const targetFov = std::clamp(state.fov, 1.0f, 179.0f);
    camera.mPosition      = position;
    camera.mForward       = forward;
    camera.mRight         = right;
    camera.mUp             = up;
    camera.mFov            = targetFov;
    if (sample.aspectRatio) camera.mAspectRatio = *sample.aspectRatio;
    camera.updateViewMatrixDependencies();

    // updateViewMatrixDependencies() owns the native matrix rebuild and may
    // replace the stack top. Write the absolute replay matrix after it runs.
    auto& view = camera.viewMatrixStack->getTop()._m.get();
    view       = ::glm::mat4x4{1.0f};
    view[0]    = {right.x, right.y, right.z, 0.0f};
    view[1]    = {up.x, up.y, up.z, 0.0f};
    view[2]    = {forward.x, forward.y, forward.z, 0.0f};
    view[3]    = {-dot(right, position), -dot(up, position), -dot(forward, position), 1.0f};
}

bool applyCurrentCameraTimelineState(
    mce::Camera*              setupCamera,
    LevelRendererCamera*     levelCamera,
    char const*              stage,
    CameraApplicationStage   applicationStage,
    bool                     acknowledge
) noexcept {
    auto const context = keyframe::currentCameraTimelineRenderContext();
    if (!context) return false;

    bool operatorOverride = false;
    auto sample = resolveSample(*context, operatorOverride);
    if (!sample) {
        auto& logged = gMissingSampleLogged[sourceIndex(context->source)];
        if (!logged.exchange(true, std::memory_order_acq_rel)) {
            Playback::getInstance().getSelf().getLogger().debug(
                "Camera timeline has no sample at {} boundary; native camera remains active "
                "(source={}, tick={}/{})",
                stage,
                sourceName(context->source),
                context->time.numerator,
                context->time.denominator
            );
        }
        return false;
    }

    mce::Camera* clientCamera = nullptr;
    if (auto client = ll::service::getClientInstance()) clientCamera = &client->getCamera();

    if (setupCamera) applyCameraState(*setupCamera, *sample);
    if (levelCamera) {
        auto& worldCamera = levelCamera->mWorldSpaceCamera.get();
        if (&worldCamera != setupCamera) applyCameraState(worldCamera, *sample);
        levelCamera->mCameraPos = Vec3{sample->state.x, sample->state.y, sample->state.z};
        auto const& forward = worldCamera.mForward.get();
        levelCamera->mCameraTargetPos = Vec3{
            sample->state.x + forward.x,
            sample->state.y + forward.y,
            sample->state.z + forward.z,
        };
    }
    if (clientCamera && clientCamera != setupCamera) applyCameraState(*clientCamera, *sample);
    if (!setupCamera && !levelCamera && !clientCamera) return false;

    if (acknowledge && levelCamera && context->appliedFlag) {
        context->appliedFlag->store(true, std::memory_order_release);
    }

    mce::Camera* diagnosticCamera = clientCamera ? clientCamera : setupCamera;
    if (!diagnosticCamera && levelCamera) diagnosticCamera = &levelCamera->mWorldSpaceCamera.get();
    auto& diagnosticLogged = gDiagnosticLogged[sourceIndex(context->source)][static_cast<size_t>(applicationStage)];
    if (diagnosticCamera && !diagnosticLogged.exchange(true, std::memory_order_acq_rel)) {
        auto const& state = sample->state;
        auto const& view = diagnosticCamera->viewMatrixStack->getTop()._m.get();
        auto const client = ll::service::getClientInstance();
        auto const player = client ? client->getLocalPlayer() : nullptr;
        auto const playerPosition = player ? player->getPosition() : Vec3{};
        auto const levelPosition = levelCamera ? levelCamera->mCameraPos.get() : Vec3{};
        Playback::getInstance().getSelf().getLogger().info(
            "Camera timeline diagnostic (stage={}, source={}, token={}, tick={}/{}, sample=({}, {}, {}, yaw={}, pitch={}, fov={}), camera=({}, {}, {}), forward=({}, {}, {}), matrixT=({}, {}, {}), player=({}, {}, {}), level=({}, {}, {}))",
            stage,
            sourceName(context->source),
            context->renderToken,
            context->time.numerator,
            context->time.denominator,
            state.x,
            state.y,
            state.z,
            state.yaw,
            state.pitch,
            state.fov,
            diagnosticCamera->mPosition->x,
            diagnosticCamera->mPosition->y,
            diagnosticCamera->mPosition->z,
            diagnosticCamera->mForward->x,
            diagnosticCamera->mForward->y,
            diagnosticCamera->mForward->z,
            view[3].x,
            view[3].y,
            view[3].z,
            playerPosition.x,
            playerPosition.y,
            playerPosition.z,
            levelPosition.x,
            levelPosition.y,
            levelPosition.z
        );
    }
    return true;
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

    // Bedrock may pass a transient camera here, so update both this instance
    // and IClientInstance's final camera after native setup.
    (void)applyCurrentCameraTimelineState(
        &camera,
        static_cast<LevelRendererCamera*>(static_cast<LevelRendererPlayer*>(this)),
        "setupCamera.final",
        CameraApplicationStage::SetupFinal,
        true
    );
}

std::optional<keyframe::CameraTimelineSample> resolveSample(
    keyframe::CameraTimelineRenderContext const& context,
    bool&                                        operatorOverride
) noexcept {
    operatorOverride = false;
    if (context.source == keyframe::CameraTimelineSource::Preview) {
        if (auto const preview = keyframe::currentPreviewCameraOverride()) {
            operatorOverride = true;
            return keyframe::CameraTimelineSample{
                *preview,
                context.sample ? context.sample->aspectRatio : std::optional<float>{},
            };
        }
    }
    return context.sample ? context.sample : keyframe::sampleCameraTimeline(context.source, context.time);
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraFrameHook,
    ll::memory::HookPriority::Highest,
    GameRenderer,
    &GameRenderer::renderCurrentFrame,
    void,
    float partialTick
) {
    (void)applyCurrentCameraTimelineState(
        nullptr,
        nullptr,
        "renderCurrentFrame.pre",
        CameraApplicationStage::FramePre,
        false
    );
    origin(partialTick);
    (void)applyCurrentCameraTimelineState(
        nullptr,
        nullptr,
        "renderCurrentFrame.post",
        CameraApplicationStage::FramePost,
        false
    );
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
    auto renderObject = origin(screenContext, clientSubId);
    auto const context = keyframe::currentCameraTimelineRenderContext();
    if (!context) return renderObject;

    bool operatorOverride = false;
    auto sample = resolveSample(*context, operatorOverride);
    if (!sample) return renderObject;

    auto const& camera  = this->mWorldSpaceCamera.get();
    auto const& forward = camera.mForward.get();
    auto&       view    = renderObject.mViewData.get();
    view.mCameraPos = ::glm::vec3{sample->state.x, sample->state.y, sample->state.z};
    view.mCameraTargetPos = ::glm::vec3{
        sample->state.x + forward.x,
        sample->state.y + forward.y,
        sample->state.z + forward.z,
    };

    if (context->appliedFlag) context->appliedFlag->store(true, std::memory_order_release);
    auto& logged = gDiagnosticLogged[sourceIndex(context->source)]
                                     [static_cast<size_t>(CameraApplicationStage::ViewRenderObject)];
    if (!logged.exchange(true, std::memory_order_acq_rel)) {
        Playback::getInstance().getSelf().getLogger().info(
            "Camera timeline ViewRenderObject diagnostic (source={}, token={}, position=({}, {}, {}), target=({}, {}, {}))",
            sourceName(context->source),
            context->renderToken,
            view.mCameraPos->x,
            view.mCameraPos->y,
            view.mCameraPos->z,
            view.mCameraTargetPos->x,
            view.mCameraTargetPos->y,
            view.mCameraTargetPos->z
        );
    }
    return renderObject;
}

std::atomic_bool gInstalled{false};

} // namespace

bool hookCameraRender(bool enable) {
    struct HookState {
        bool setupCamera{};
        bool viewRenderObject{};
        bool gameFrame{};
    };
    static HookState state;

    if (enable) {
        for (auto& flag : gMissingSampleLogged) flag.store(false, std::memory_order_release);
        for (auto& sourceFlags : gDiagnosticLogged) {
            for (auto& flag : sourceFlags) flag.store(false, std::memory_order_release);
        }
        if (!state.setupCamera) state.setupCamera = ReplayCameraSetupHook::hook() == 0;
        if (!state.viewRenderObject) state.viewRenderObject = ReplayCameraViewRenderObjectHook::hook() == 0;
        if (!state.gameFrame) state.gameFrame = ReplayCameraFrameHook::hook() == 0;
        bool const installed = state.setupCamera && state.viewRenderObject && state.gameFrame;
        gInstalled.store(installed, std::memory_order_release);
        if (installed) {
            Playback::getInstance().getSelf().getLogger().info(
                "Camera render hooks installed (setupCamera={}, createViewRenderObject={}, renderCurrentFrame={})",
                state.setupCamera,
                state.viewRenderObject,
                state.gameFrame
            );
        }
        return installed;
    }

    gInstalled.store(false, std::memory_order_release);
    for (auto& sourceFlags : gDiagnosticLogged) {
        for (auto& flag : sourceFlags) flag.store(false, std::memory_order_release);
    }
    if (state.gameFrame && ReplayCameraFrameHook::unhook()) state.gameFrame = false;
    if (state.viewRenderObject && ReplayCameraViewRenderObjectHook::unhook()) state.viewRenderObject = false;
    if (state.setupCamera && ReplayCameraSetupHook::unhook()) state.setupCamera = false;
    return !state.setupCamera && !state.viewRenderObject && !state.gameFrame;
}

bool isCameraRenderInstalled() { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::editor::renderer
