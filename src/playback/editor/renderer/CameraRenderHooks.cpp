#include "CameraRenderHooks.h"

#include "playback/Playback.h"
#include "playback/editor/keyframe/CameraTimelineRegistry.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

namespace playback::editor::renderer {

namespace {

constexpr float RadiansPerDegree = 3.14159265358979323846f / 180.0f;

std::array<std::atomic_bool, 2>     gMissingSampleLogged{};
std::array<std::atomic_bool, 2>     gAppliedInfoLogged{};

size_t sourceIndex(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? 1U : 0U;
}

char const* sourceName(keyframe::CameraTimelineSource source) noexcept {
    return source == keyframe::CameraTimelineSource::Export ? "export" : "preview";
}

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

    camera.mPosition = ::glm::vec3{state.x, state.y, state.z};
    camera.mForward  = forward;
    camera.mRight    = right;
    camera.mUp       = up;
    camera.mFov      = std::clamp(state.fov, 1.0f, 179.0f);
    if (sample.aspectRatio) camera.mAspectRatio = *sample.aspectRatio;
    camera.updateViewMatrixDependencies();
}

bool applyCurrentCameraTimelineState(mce::Camera* setupCamera, char const* stage) noexcept {
    auto const context = keyframe::currentCameraTimelineRenderContext();
    if (!context) return false;

    std::optional<keyframe::CameraTimelineSample> sample;
    bool                                          operatorOverride = false;
    if (context->source == keyframe::CameraTimelineSource::Preview) {
        if (auto const preview = keyframe::currentPreviewCameraOverride()) {
            sample = keyframe::CameraTimelineSample{
                *preview,
                context->sample ? context->sample->aspectRatio : std::optional<float>{},
            };
            operatorOverride = true;
        }
    }
    if (!sample) {
        sample = context->sample
            ? context->sample
            : keyframe::sampleCameraTimeline(context->source, context->time);
    }
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
    if (clientCamera && clientCamera != setupCamera) applyCameraState(*clientCamera, *sample);
    if (!setupCamera && !clientCamera) return false;

    if (context->appliedFlag) context->appliedFlag->store(true, std::memory_order_release);

    if (!gAppliedInfoLogged[sourceIndex(context->source)].exchange(true, std::memory_order_acq_rel)) {
        auto const& state = sample->state;
        Playback::getInstance().getSelf().getLogger().info(
            "Camera timeline first application (stage={}, source={}, tick={}/{}, position=({}, {}, {}), yaw={}, pitch={}, fov={}, override={})",
            stage,
            sourceName(context->source),
            context->time.numerator,
            context->time.denominator,
            state.x,
            state.y,
            state.z,
            state.yaw,
            state.pitch,
            state.fov,
            operatorOverride
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
    (void)applyCurrentCameraTimelineState(&camera, "setupCamera");
}

LL_TYPE_INSTANCE_HOOK(
    ReplayCameraFrameHook,
    ll::memory::HookPriority::Highest,
    GameRenderer,
    &GameRenderer::renderCurrentFrame,
    void,
    float partialTick
) {
    // Some Bedrock render paths consume the client camera before entering the
    // level renderer. Seed the sampled state before the native pass, then
    // apply it again after native setup so paths that rebuild the camera still
    // end with the timeline pose.
    (void)applyCurrentCameraTimelineState(nullptr, "renderCurrentFrame.pre");
    origin(partialTick);
    (void)applyCurrentCameraTimelineState(nullptr, "renderCurrentFrame");
}

std::atomic_bool gInstalled{false};

} // namespace

bool hookCameraRender(bool enable) {
    struct HookState {
        bool setupCamera{};
        bool gameFrame{};
    };
    static HookState state;

    if (enable) {
        for (auto& flag : gMissingSampleLogged) flag.store(false, std::memory_order_release);
        for (auto& flag : gAppliedInfoLogged) flag.store(false, std::memory_order_release);
        if (!state.setupCamera) state.setupCamera = ReplayCameraSetupHook::hook() == 0;
        if (!state.gameFrame) state.gameFrame = ReplayCameraFrameHook::hook() == 0;
        bool const installed = state.setupCamera && state.gameFrame;
        gInstalled.store(installed, std::memory_order_release);
        if (installed) {
            Playback::getInstance().getSelf().getLogger().info(
                "Camera render hooks installed (setupCamera={}, renderCurrentFrame={})",
                state.setupCamera,
                state.gameFrame
            );
        }
        return installed;
    }

    gInstalled.store(false, std::memory_order_release);
    for (auto& flag : gAppliedInfoLogged) flag.store(false, std::memory_order_release);
    if (state.gameFrame && ReplayCameraFrameHook::unhook()) state.gameFrame = false;
    if (state.setupCamera && ReplayCameraSetupHook::unhook()) state.setupCamera = false;
    return !state.setupCamera && !state.gameFrame;
}

bool isCameraRenderInstalled() { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::editor::renderer
