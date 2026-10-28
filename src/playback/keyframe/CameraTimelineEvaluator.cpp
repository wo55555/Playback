#include "CameraTimelineEvaluator.h"

#include "CameraKeyframeHandler.h"
#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback::keyframe {

namespace {

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

bool isKeyframeCamera(state::editing::model::CameraEntity const& camera) noexcept {
    return camera.enabled && !camera.keysByTick.empty();
}

void applyRotationShake(CameraRenderState& state, state::editing::model::CameraShake const& shake, long double tick) {
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
    state::editing::model::EditorStateExt project,
    std::optional<std::string>            cameraOverride,
    std::optional<std::string>            cameraFallback,
    bool                                  holdLastKeyframe,
    std::vector<int>                      dimensionTransitionTicks,
    FirstPersonSampler                    firstPersonSampler
)
: mProject(std::move(project)),
  mCameraOverride(std::move(cameraOverride)),
  mCameraFallback(std::move(cameraFallback)),
  mHoldLastKeyframe(holdLastKeyframe),
  mDimensionTransitionTicks(std::move(dimensionTransitionTicks)),
  mFirstPersonSampler(std::move(firstPersonSampler)) {
    std::erase_if(mDimensionTransitionTicks, [this](int tick) { return tick < 0 || tick > mProject.totalTicks; });
    std::sort(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end());
    mDimensionTransitionTicks.erase(
        std::unique(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end()),
        mDimensionTransitionTicks.end()
    );

    for (auto& camera : mProject.cameras) {
        if (camera.keysByTick.empty()) continue;

        std::vector<CameraTrackSegment> segments;
        KeyframeTrack::KeyMap           segmentKeys;
        size_t                          segmentIndex  = dimensionSegmentForTick(camera.keysByTick.begin()->first);
        auto                            appendSegment = [&] {
            if (segmentKeys.empty()) return;
            segments.emplace_back(
                CameraTrackSegment{
                    segmentIndex,
                    segmentKeys.rbegin()->first,
                    KeyframeTrack{segmentKeys},
                }
            );
            segmentKeys.clear();
        };

        for (auto const& [tick, keyframe] : camera.keysByTick) {
            auto const keySegment = dimensionSegmentForTick(tick);
            if (keySegment != segmentIndex) {
                appendSegment();
                segmentIndex = keySegment;
            }
            segmentKeys.emplace(tick, keyframe);
        }
        appendSegment();
        mKeyframeTracks.emplace(camera.id, std::move(segments));
    }
}

size_t CameraTimelineEvaluator::dimensionSegmentForTick(long double tick) const noexcept {
    auto const boundary = std::lower_bound(
        mDimensionTransitionTicks.begin(),
        mDimensionTransitionTicks.end(),
        tick,
        [](int transitionTick, long double value) { return static_cast<long double>(transitionTick) < value; }
    );
    return static_cast<size_t>(std::distance(mDimensionTransitionTicks.begin(), boundary));
}

namespace {

state::editing::model::CameraEntity const*
findKeyframeCamera(state::editing::model::EditorStateExt const& project, std::string_view id) {
    if (id.empty()) return nullptr;
    auto const match = std::ranges::find(project.cameras, id, &state::editing::model::CameraEntity::id);
    return match == project.cameras.end() || !isKeyframeCamera(*match) ? nullptr : &*match;
}

bool isFirstPersonId(std::string_view id) noexcept { return id == state::editing::model::kFirstPersonCameraId; }

} // namespace

state::editing::model::CameraEntity const* CameraTimelineEvaluator::fallbackCamera() const {
    if (mCameraFallback) {
        if (auto const* camera = findKeyframeCamera(mProject, *mCameraFallback)) return camera;
    }
    auto const firstRenderable = std::ranges::find_if(mProject.cameras, isKeyframeCamera);
    return firstRenderable == mProject.cameras.end() ? nullptr : &*firstRenderable;
}

CameraTimelineEvaluator::CameraChoice CameraTimelineEvaluator::cameraForTick(int64_t tick) const {
    if (mCameraOverride && !mCameraOverride->empty()) {
        if (isFirstPersonId(*mCameraOverride)) return {nullptr, true};
        if (auto const* camera = findKeyframeCamera(mProject, *mCameraOverride)) return {camera, false};
    }

    for (auto const& segment : mProject.sequence) {
        if (tick < segment.startTick || tick >= segment.endTick || segment.cameraId.empty()) continue;
        if (isFirstPersonId(segment.cameraId)) return {nullptr, true};
        if (auto const* camera = findKeyframeCamera(mProject, segment.cameraId)) return {camera, false};
        break;
    }

    if (mCameraFallback && isFirstPersonId(*mCameraFallback)) return {nullptr, true};
    return {fallbackCamera(), false};
}

bool CameraTimelineEvaluator::isFirstPersonAt(int64_t tick) const { return cameraForTick(tick).firstPerson; }

std::optional<CameraRenderState>
CameraTimelineEvaluator::sampleCamera(state::editing::model::CameraEntity const& camera, long double tick) const {
    if (camera.keysByTick.empty()) return std::nullopt;

    auto const tracks = mKeyframeTracks.find(camera.id);
    if (tracks == mKeyframeTracks.end()) return std::nullopt;
    auto const segmentIndex = dimensionSegmentForTick(tick);
    auto const segment      = std::ranges::find(tracks->second, segmentIndex, &CameraTrackSegment::dimensionSegment);
    if (segment == tracks->second.end()) return std::nullopt;

    auto change = segment->track.createChange(tick);
    if (!change && mHoldLastKeyframe && tick > static_cast<long double>(segment->lastTick)) {
        change = segment->track.createChange(segment->lastTick);
    }
    if (!change) return std::nullopt;

    CameraRenderStateHandler handler;
    change->apply(handler);
    auto state = handler.state();
    if (!state) return std::nullopt;
    applyRotationShake(*state, camera.shake.value_or(state::editing::model::CameraShake{}), tick);
    return state;
}

std::optional<CameraTimelineEvaluation> CameraTimelineEvaluator::sample(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto choice = cameraForTick(time.floorTick());
    if (choice.firstPerson) {
        if (mFirstPersonSampler) {
            if (auto state = mFirstPersonSampler(time)) {
                return CameraTimelineEvaluation{*state, std::string{state::editing::model::kFirstPersonCameraId}};
            }
        }
        // The recorded player has no pose here (before it spawned, between dimensions): keep a keyframed view.
        choice.camera = fallbackCamera();
    }
    auto const* camera = choice.camera;
    if (!camera) return std::nullopt;
    auto state = sampleCamera(*camera, time.value());
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id}} : std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const {
    if (cameraId.empty() || !time.isValid()) return std::nullopt;
    auto const camera =
        std::ranges::find_if(mProject.cameras, [&](auto const& candidate) { return candidate.id == cameraId; });
    if (camera == mProject.cameras.end() || !isKeyframeCamera(*camera)) return std::nullopt;
    auto state = sampleCamera(*camera, time.value());
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id}} : std::nullopt;
}

} // namespace playback::keyframe
