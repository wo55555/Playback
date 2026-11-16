#include "CameraTimelineEvaluator.h"

#include "CameraKeyframeHandler.h"
#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace playback::keyframe {

namespace {

using state::editing::model::CameraEntity;
using state::editing::model::CameraTrackKind;

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

bool isUsableCamera(CameraEntity const& camera) noexcept { return camera.enabled && !camera.keysByTick.empty(); }

bool isKeyframeCamera(CameraEntity const& camera) noexcept {
    return isUsableCamera(camera) && camera.kind == CameraTrackKind::Keyframe;
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
    state.x     += std::sin(phase * 1.37f) * shake.positionAmplitude * envelope;
    state.y     += std::cos(phase * 1.19f) * shake.positionAmplitude * envelope;
    state.z     += std::sin(phase * 0.83f) * shake.positionAmplitude * envelope;
    state.yaw   += std::sin(phase * 1.11f) * shake.rotationAmplitude * envelope;
    state.pitch += std::cos(phase * 0.91f) * shake.rotationAmplitude * envelope;
}

bool coversTick(CameraEntity const& camera, int64_t tick) noexcept {
    return isUsableCamera(camera) && tick >= camera.keysByTick.begin()->first
        && tick <= camera.keysByTick.rbegin()->first;
}

std::string subjectAt(CameraEntity const& camera, int64_t tick) {
    auto key = camera.keysByTick.upper_bound(static_cast<int>(tick));
    if (key == camera.keysByTick.begin()) return {};
    return std::prev(key)->second.subject;
}

} // namespace

CameraTimelineEvaluator::CameraTimelineEvaluator(
    state::editing::model::EditorStateExt project,
    std::optional<std::string>            manualCameraId,
    bool                                  holdOutsideTracks,
    std::vector<int>                      dimensionTransitionTicks,
    SubjectViewSampler                    subjectSampler
)
: mProject(std::move(project)),
  mManualCameraId(std::move(manualCameraId)),
  mHoldOutsideTracks(holdOutsideTracks),
  mDimensionTransitionTicks(std::move(dimensionTransitionTicks)),
  mSubjectSampler(std::move(subjectSampler)) {
    std::erase_if(mDimensionTransitionTicks, [this](int tick) { return tick < 0 || tick > mProject.totalTicks; });
    std::sort(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end());
    mDimensionTransitionTicks.erase(
        std::unique(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end()),
        mDimensionTransitionTicks.end()
    );

    for (auto& camera : mProject.cameras) {
        if (camera.keysByTick.empty() || camera.kind != CameraTrackKind::Keyframe) continue;

        std::vector<CameraTrackSegment> segments;
        KeyframeTrack::KeyMap           segmentKeys;
        size_t                          segmentIndex  = dimensionSegmentForTick(camera.keysByTick.begin()->first);
        auto                            appendSegment = [&] {
            if (segmentKeys.empty()) return;
            segments.emplace_back(
                CameraTrackSegment{
                    segmentIndex,
                    segmentKeys.begin()->first,
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

std::optional<CameraRenderState>
CameraTimelineEvaluator::sampleKeyframes(CameraEntity const& camera, long double tick, bool clamp) const {
    auto const tracks = mKeyframeTracks.find(camera.id);
    if (tracks == mKeyframeTracks.end()) return std::nullopt;
    auto const segment =
        std::ranges::find(tracks->second, dimensionSegmentForTick(tick), &CameraTrackSegment::dimensionSegment);
    if (segment == tracks->second.end()) return std::nullopt;

    auto const keyTick = clamp ? std::clamp(
                                     tick,
                                     static_cast<long double>(segment->firstTick),
                                     static_cast<long double>(segment->lastTick)
                                 )
                               : tick;
    auto change = segment->track.createChange(keyTick);
    if (!change) return std::nullopt;

    CameraRenderStateHandler handler;
    change->apply(handler);
    auto state = handler.state();
    if (!state) return std::nullopt;
    applyRotationShake(*state, camera.shake.value_or(state::editing::model::CameraShake{}), tick);
    return state;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleTrack(CameraEntity const& camera, visuals::ReplaySampleTime const& time) const {
    if (camera.kind == CameraTrackKind::Keyframe) {
        auto state = sampleKeyframes(camera, time.value(), true);
        if (!state) return std::nullopt;
        return CameraTimelineEvaluation{*state, camera.id, camera.kind};
    }
    if (!mSubjectSampler) return std::nullopt;
    auto state =
        mSubjectSampler(time, subjectAt(camera, time.floorTick()), camera.kind == CameraTrackKind::ThirdPerson);
    if (!state) return std::nullopt;
    return CameraTimelineEvaluation{*state, camera.id, camera.kind};
}

CameraShot CameraTimelineEvaluator::shotAt(int64_t tick) const {
    for (auto const& camera : mProject.cameras) {
        if (!coversTick(camera, tick)) continue;
        if (camera.kind == CameraTrackKind::Keyframe) {
            // A keyframe track only owns the dimensions it has keys in.
            auto const tracks = mKeyframeTracks.find(camera.id);
            if (tracks == mKeyframeTracks.end()
                || std::ranges::find(
                       tracks->second,
                       dimensionSegmentForTick(static_cast<long double>(tick)),
                       &CameraTrackSegment::dimensionSegment
                   ) == tracks->second.end()) {
                continue;
            }
            return CameraShot{&camera, {}};
        }
        return CameraShot{&camera, subjectAt(camera, tick)};
    }
    return {};
}

std::optional<CameraTimelineEvaluation> CameraTimelineEvaluator::sample(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const tick = time.floorTick();

    for (auto const& camera : mProject.cameras) {
        if (!coversTick(camera, tick)) continue;
        // Without a sampler the caller views person tracks itself (the preview follows the subject natively).
        if (camera.kind != CameraTrackKind::Keyframe && !mSubjectSampler) return std::nullopt;
        // A track with nothing to show here (no keys in this dimension, subject absent) yields to lower ones.
        if (auto evaluation = sampleTrack(camera, time)) return evaluation;
    }

    if (mManualCameraId && !mManualCameraId->empty()) {
        if (auto evaluation = sampleCameraById(*mManualCameraId, time)) return evaluation;
    }
    if (!mHoldOutsideTracks) return std::nullopt;
    auto const fallback = std::ranges::find_if(mProject.cameras, isKeyframeCamera);
    if (fallback == mProject.cameras.end()) return std::nullopt;
    auto state = sampleKeyframes(*fallback, time.value(), true);
    return state ? std::optional<CameraTimelineEvaluation>{{*state, fallback->id, fallback->kind}} : std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleKeyframePath(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const tick = time.floorTick();
    for (auto const& camera : mProject.cameras) {
        if (camera.kind != CameraTrackKind::Keyframe || !coversTick(camera, tick)) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true)) {
            return CameraTimelineEvaluation{*state, camera.id, camera.kind};
        }
    }
    return std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const {
    if (cameraId.empty() || !time.isValid()) return std::nullopt;
    auto const camera = std::ranges::find(mProject.cameras, cameraId, &CameraEntity::id);
    if (camera == mProject.cameras.end() || !isKeyframeCamera(*camera)) return std::nullopt;
    auto state = sampleKeyframes(*camera, time.value(), true);
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id, camera->kind}} : std::nullopt;
}

} // namespace playback::keyframe
