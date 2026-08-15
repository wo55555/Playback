#include "CameraTimelineEvaluator.h"

#include "CameraKeyframeHandler.h"
#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback::editor::keyframe {

namespace {

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

bool isKeyframeCamera(editing::model::CameraEntity const& camera) noexcept {
    return camera.enabled && !camera.keysByTick.empty();
}

void applyRotationShake(CameraRenderState& state, editing::model::CameraShake const& shake, long double tick) {
    if (shake.endTick <= shake.startTick || tick < shake.startTick || tick > shake.endTick) return;
    float const elapsed  = static_cast<float>(tick - static_cast<long double>(shake.startTick));
    float const phase    = elapsed * shake.frequency * 6.2831853071795864769f;
    float const envelope = clampUnit(
        static_cast<float>(
            (static_cast<long double>(shake.endTick) - tick) / static_cast<long double>(shake.endTick - shake.startTick)
        )
    );
    state.yaw   += std::sin(phase * 1.11f) * shake.rotationAmplitude * envelope;
    state.pitch += std::cos(phase * 0.91f) * shake.rotationAmplitude * envelope;
}

} // namespace

CameraTimelineEvaluator::CameraTimelineEvaluator(
    editing::model::EditorStateExt project,
    std::optional<std::string>     cameraOverride,
    std::optional<std::string>     cameraFallback,
    bool                           holdLastKeyframe
)
: mProject(std::move(project)),
  mCameraOverride(std::move(cameraOverride)),
  mCameraFallback(std::move(cameraFallback)),
  mHoldLastKeyframe(holdLastKeyframe) {
    for (auto& camera : mProject.cameras) {
        // keysByTick is already ordered by tick, so no sort is required.
        if (!camera.keysByTick.empty()) mKeyframeTracks.emplace(camera.id, KeyframeTrack{camera.keysByTick});
    }
}

editing::model::CameraEntity const* CameraTimelineEvaluator::cameraForTick(int64_t tick) const {
    if (mProject.cameras.empty()) return nullptr;

    auto findCamera = [&](std::string const& id) -> editing::model::CameraEntity const* {
        if (id.empty()) return nullptr;
        auto const match = std::ranges::find(mProject.cameras, id, &editing::model::CameraEntity::id);
        return match == mProject.cameras.end() || !isKeyframeCamera(*match) ? nullptr : &*match;
    };

    if (mCameraOverride && !mCameraOverride->empty()) {
        if (auto const* camera = findCamera(*mCameraOverride)) return camera;
    }

    for (auto const& segment : mProject.sequence) {
        if (tick < segment.startTick || tick >= segment.endTick || segment.cameraId.empty()) continue;
        if (auto const* camera = findCamera(segment.cameraId)) return camera;
        break;
    }

    if (mCameraFallback) {
        if (auto const* camera = findCamera(*mCameraFallback)) return camera;
    }
    auto const firstRenderable = std::ranges::find_if(mProject.cameras, isKeyframeCamera);
    return firstRenderable == mProject.cameras.end() ? nullptr : &*firstRenderable;
}

std::optional<CameraRenderState>
CameraTimelineEvaluator::sampleCamera(editing::model::CameraEntity const& camera, long double tick) const {
    if (camera.keysByTick.empty()) return std::nullopt;

    auto const track = mKeyframeTracks.find(camera.id);
    if (track == mKeyframeTracks.end()) return std::nullopt;
    auto change = track->second.createChange(tick);
    if (!change && mHoldLastKeyframe && tick > static_cast<long double>(camera.keysByTick.rbegin()->first)) {
        change = track->second.createChange(camera.keysByTick.rbegin()->first);
    }
    if (!change) return std::nullopt;

    CameraRenderStateHandler handler;
    change->apply(handler);
    auto state = handler.state();
    if (!state) return std::nullopt;
    applyRotationShake(*state, camera.shake.value_or(editing::model::CameraShake{}), tick);
    return state;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sample(functions::render::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const* camera = cameraForTick(time.floorTick());
    if (!camera) return std::nullopt;
    auto state = sampleCamera(*camera, time.value());
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id}} : std::nullopt;
}

std::optional<CameraTimelineEvaluation> CameraTimelineEvaluator::sampleCameraById(
    std::string_view                           cameraId,
    functions::render::ReplaySampleTime const& time
) const {
    if (cameraId.empty() || !time.isValid()) return std::nullopt;
    auto const camera =
        std::ranges::find_if(mProject.cameras, [&](auto const& candidate) { return candidate.id == cameraId; });
    if (camera == mProject.cameras.end() || !isKeyframeCamera(*camera)) return std::nullopt;
    auto state = sampleCamera(*camera, time.value());
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id}} : std::nullopt;
}

} // namespace playback::editor::keyframe
