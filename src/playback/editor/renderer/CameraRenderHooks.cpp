#include "CameraRenderHooks.h"

#include "playback/editor/keyframe/CameraTimelineRegistry.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace playback::editor::renderer {

namespace {

constexpr float RadiansPerDegree = 3.14159265358979323846f / 180.0f;

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

    // The controller publishes one immutable context for the current render
    // pass.  Applying after native setup makes this the sole final camera
    // authority and leaves replay/player simulation untouched.
    auto const context = keyframe::currentCameraTimelineRenderContext();
    if (!context) return;

    // Export passes carry an immutable sample from the clock publisher. This
    // keeps keyframe evaluation out of the native render call and guarantees
    // that the camera and entity pose use the same fractional replay tick.
    auto const sample = context->sample
        ? context->sample
        : keyframe::sampleCameraTimeline(context->source, context->time);
    if (!sample) return;

    applyCameraState(camera, *sample);
    if (context->appliedFlag) context->appliedFlag->store(true, std::memory_order_release);
}

std::atomic_bool gInstalled{false};

} // namespace

bool hookCameraRender(bool enable) {
    static bool installed = false;
    if (enable) {
        if (!installed) installed = ReplayCameraSetupHook::hook() == 0;
        gInstalled.store(installed, std::memory_order_release);
        return installed;
    }

    if (installed && ReplayCameraSetupHook::unhook()) installed = false;
    gInstalled.store(installed, std::memory_order_release);
    return !installed;
}

bool isCameraRenderInstalled() { return gInstalled.load(std::memory_order_acquire); }

} // namespace playback::editor::renderer
