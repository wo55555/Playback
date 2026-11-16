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
using state::editing::model::CameraKeyframe;
using state::editing::model::CameraKeyframeMode;

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

bool isUsableCamera(CameraEntity const& camera) noexcept { return camera.enabled && !camera.keysByTick.empty(); }

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

CameraKeyframe const* keyAtOrBefore(CameraEntity const& camera, int64_t tick) noexcept {
    if (!isUsableCamera(camera)) return nullptr;
    auto const upper = camera.keysByTick.upper_bound(static_cast<int>(std::min<int64_t>(tick, 2147483647LL)));
    if (upper == camera.keysByTick.begin()) return nullptr;
    return &std::prev(upper)->second;
}

std::pair<int, CameraKeyframe const*> keyEntryAtOrBefore(CameraEntity const& camera, int64_t tick) noexcept {
    if (!isUsableCamera(camera)) return {-1, nullptr};
    auto const upper = camera.keysByTick.upper_bound(static_cast<int>(std::min<int64_t>(tick, 2147483647LL)));
    if (upper == camera.keysByTick.begin()) return {-1, nullptr};
    auto const it = std::prev(upper);
    return {it->first, &it->second};
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
        if (camera.keysByTick.empty()) continue;

        std::vector<CameraTrackSegment> segments;
        KeyframeTrack::KeyMap           segmentKeys;
        size_t                          segmentIndex  = 0;
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

        // Split interpolated runs at dimension transitions and at person keyframes so splines never bridge them.
        for (auto const& [tick, keyframe] : camera.keysByTick) {
            if (keyframe.mode != CameraKeyframeMode::Interpolated) {
                appendSegment();
                continue;
            }
            auto const keySegment = dimensionSegmentForTick(tick);
            if (segmentKeys.empty()) {
                segmentIndex = keySegment;
            } else if (keySegment != segmentIndex) {
                appendSegment();
                segmentIndex = keySegment;
            }
            segmentKeys.emplace(tick, keyframe);
        }
        appendSegment();
        if (!segments.empty()) mKeyframeTracks.emplace(camera.id, std::move(segments));
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

std::optional<CameraRenderState> CameraTimelineEvaluator::sampleKeyframes(
    CameraEntity const& camera,
    long double         tick,
    bool                clamp,
    bool                allowBeforeFirst
) const {
    auto const tracks = mKeyframeTracks.find(camera.id);
    if (tracks == mKeyframeTracks.end() || tracks->second.empty()) return std::nullopt;
    auto const curSegment = dimensionSegmentForTick(tick);

    CameraTrackSegment const* matched = nullptr;
    for (auto const& segment : tracks->second) {
        if (segment.dimensionSegment != curSegment) continue;
        if (static_cast<long double>(segment.firstTick) <= tick) {
            matched = &segment;
        } else if (allowBeforeFirst && !matched) {
            matched = &segment;
            break;
        }
    }
    if (!matched) return std::nullopt;

    auto const keyTick = clamp ? std::clamp(
                                     tick,
                                     static_cast<long double>(matched->firstTick),
                                     static_cast<long double>(matched->lastTick)
                                 )
                               : tick;
    auto change = matched->track.createChange(keyTick);
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
    auto const [keyTick, key] = keyEntryAtOrBefore(camera, time.floorTick());
    if (!key) return std::nullopt;
    if (key->mode == CameraKeyframeMode::Interpolated) {
        // Interpolated keyframes hold only through the end of their own dimension interval.
        if (dimensionSegmentForTick(static_cast<long double>(keyTick)) != dimensionSegmentForTick(time.value())) {
            return std::nullopt;
        }
        auto state = sampleKeyframes(camera, time.value(), true, false);
        if (!state) return std::nullopt;
        return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
    }
    if (!mSubjectSampler) return std::nullopt;
    auto state = mSubjectSampler(time, key->subject, key->mode == CameraKeyframeMode::ThirdPerson);
    if (!state) return std::nullopt;
    return CameraTimelineEvaluation{*state, camera.id, key->mode};
}

CameraShot CameraTimelineEvaluator::shotAt(int64_t tick) const {
    auto const curSegment = dimensionSegmentForTick(static_cast<long double>(tick));
    for (auto const& camera : mProject.cameras) {
        auto const [keyTick, key] = keyEntryAtOrBefore(camera, tick);
        if (!key) continue;
        if (key->mode == CameraKeyframeMode::Interpolated) {
            if (dimensionSegmentForTick(static_cast<long double>(keyTick)) != curSegment) continue;
            return CameraShot{&camera, CameraKeyframeMode::Interpolated, {}};
        }
        // Person keyframes extend across dimension boundaries until the next keyframe or end of replay.
        return CameraShot{&camera, key->mode, key->subject};
    }
    return {};
}

std::optional<CameraTimelineEvaluation> CameraTimelineEvaluator::sample(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const tick       = time.floorTick();
    auto const curSegment = dimensionSegmentForTick(time.value());

    for (auto const& camera : mProject.cameras) {
        auto const [keyTick, key] = keyEntryAtOrBefore(camera, tick);
        if (!key) continue;
        if (key->mode == CameraKeyframeMode::Interpolated
            && dimensionSegmentForTick(static_cast<long double>(keyTick)) != curSegment) {
            continue;
        }
        // Without a sampler the caller views person keyframes itself (the preview follows the subject natively).
        if (key->mode != CameraKeyframeMode::Interpolated && !mSubjectSampler) return std::nullopt;
        // A track with nothing to show here (subject absent, etc.) yields to lower ones.
        if (auto evaluation = sampleTrack(camera, time)) return evaluation;
    }

    if (mManualCameraId && !mManualCameraId->empty()) {
        if (auto evaluation = sampleCameraById(*mManualCameraId, time)) return evaluation;
    }
    if (!mHoldOutsideTracks) return std::nullopt;
    for (auto const& camera : mProject.cameras) {
        if (!isUsableCamera(camera)) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true, true)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
        }
    }
    return std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleKeyframePath(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const tick       = time.floorTick();
    auto const curSegment = dimensionSegmentForTick(time.value());
    for (auto const& camera : mProject.cameras) {
        auto const [keyTick, key] = keyEntryAtOrBefore(camera, tick);
        if (!key || key->mode != CameraKeyframeMode::Interpolated) continue;
        if (dimensionSegmentForTick(static_cast<long double>(keyTick)) != curSegment) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true, false)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
        }
    }
    return std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const {
    if (cameraId.empty() || !time.isValid()) return std::nullopt;
    auto const camera = std::ranges::find(mProject.cameras, cameraId, &CameraEntity::id);
    if (camera == mProject.cameras.end() || !isUsableCamera(*camera)) return std::nullopt;
    auto state = sampleKeyframes(*camera, time.value(), true, true);
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id, CameraKeyframeMode::Interpolated}}
                 : std::nullopt;
}

} // namespace playback::keyframe
