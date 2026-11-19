#include "CameraTimelineEvaluator.h"

#include "CameraKeyframeHandler.h"
#include "KeyframeTrack.h"
#include "playback/state/editing/ClipOps.h"

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
  mAnyCameraHasClips(std::ranges::any_of(mProject.cameras, [](auto const& c) { return !c.clips.empty(); })),
  mDimensionTransitionTicks(std::move(dimensionTransitionTicks)),
  mSubjectSampler(std::move(subjectSampler)) {
    std::erase_if(mDimensionTransitionTicks, [this](int tick) { return tick < 0 || tick > mProject.totalTicks; });
    std::sort(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end());
    mDimensionTransitionTicks.erase(
        std::unique(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end()),
        mDimensionTransitionTicks.end()
    );
    buildSpeedSpans();

    for (auto& camera : mProject.cameras) {
        if (camera.keysByTick.empty()) continue;

        std::vector<CameraTrackSegment> segments;
        KeyframeTrack::KeyMap           segmentKeys;
        size_t                          segmentIndex  = 0;
        auto                            appendSegment = [&] {
            if (segmentKeys.empty()) return;
            int const                    firstTick = segmentKeys.begin()->first;
            int const                    lastTick  = segmentKeys.rbegin()->first;
            std::vector<SegmentInterval> intervals;
            bool                         hasUnscaledIntervals = false;

            if (segmentKeys.size() >= 2) {
                intervals.reserve(segmentKeys.size() - 1);
                long double rCursor = static_cast<long double>(firstTick);
                for (auto it = segmentKeys.begin(), next = std::next(it); next != segmentKeys.end();
                     it = next, ++next) {
                    int const         tk     = it->first;
                    int const         tkNext = next->first;
                    long double const span   = static_cast<long double>(tkNext - tk);
                    if (it->second.ignoreClipSpeed) {
                        hasUnscaledIntervals     = true;
                        long double const uStart = unscaledTicksAt(rCursor);
                        long double const uEnd   = uStart + span;
                        long double const rEnd   = replayTickForUnscaled(uEnd);
                        intervals.push_back(SegmentInterval{rCursor, rEnd, uStart, tk, tkNext, true});
                        rCursor = rEnd;
                    } else {
                        long double const rStart = rCursor;
                        long double const rEnd   = (rCursor == static_cast<long double>(tk))
                                                     ? static_cast<long double>(tkNext)
                                                     : (rStart + span);
                        intervals.push_back(SegmentInterval{rStart, rEnd, 0.0L, tk, tkNext, false});
                        rCursor = rEnd;
                    }
                }
            }

            segments.emplace_back(
                CameraTrackSegment{
                    segmentIndex,
                    firstTick,
                    lastTick,
                    KeyframeTrack{segmentKeys},
                    std::move(intervals),
                    hasUnscaledIntervals,
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

bool CameraTimelineEvaluator::cameraCoversTick(CameraEntity const& camera, int64_t tick) const noexcept {
    if (!camera.enabled) return false;
    if (!mAnyCameraHasClips || mProject.totalTicks <= 0) return true;
    int const clampedTick = static_cast<int>(std::clamp<int64_t>(tick, 0, mProject.totalTicks));
    return state::editing::ClipOps::cameraHasActiveClipAt(camera, clampedTick, mProject.totalTicks, false);
}

void CameraTimelineEvaluator::buildSpeedSpans() {
    mSpeedSpans.clear();
    mSpeedSpanEndU = 0.0L;

    auto const slices = state::editing::ClipOps::collectCompositedSlices(mProject, false, mDimensionTransitionTicks);

    int horizonTick = std::max(0, mProject.totalTicks);
    for (auto const& camera : mProject.cameras) {
        if (!camera.keysByTick.empty()) {
            horizonTick = std::max(horizonTick, camera.keysByTick.rbegin()->first);
        }
    }
    for (auto const& slice : slices) {
        if (slice.inTick < slice.outTick) horizonTick = std::max(horizonTick, slice.outTick);
    }
    if (horizonTick <= 0) return;

    std::vector<int> boundaries{0, horizonTick};
    for (auto const& slice : slices) {
        int const start = std::clamp(slice.inTick, 0, horizonTick);
        int const end   = std::clamp(slice.outTick, 0, horizonTick);
        if (start < end) {
            boundaries.push_back(start);
            boundaries.push_back(end);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    long double cumU = 0.0L;
    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        int const bStart = boundaries[i];
        int const bEnd   = boundaries[i + 1];
        if (bStart >= bEnd) continue;

        long double speed = 1.0L;
        for (auto const& slice : slices) {
            if (slice.inTick < slice.outTick && slice.inTick <= bStart && bEnd <= slice.outTick) {
                speed = static_cast<long double>(state::editing::model::clampClipSpeed(slice.speed));
                break;
            }
        }

        if (!mSpeedSpans.empty() && mSpeedSpans.back().endTick == bStart && mSpeedSpans.back().speed == speed) {
            mSpeedSpans.back().endTick = bEnd;
        } else {
            mSpeedSpans.push_back(SpeedSpan{bStart, bEnd, speed, cumU});
        }
        cumU += static_cast<long double>(bEnd - bStart) / speed;
    }
    mSpeedSpanEndU = cumU;
}

long double CameraTimelineEvaluator::unscaledTicksAt(long double replayTick) const noexcept {
    if (mSpeedSpans.empty()) return replayTick;
    if (replayTick <= static_cast<long double>(mSpeedSpans.front().startTick)) {
        return mSpeedSpans.front().startUnscaled
             + (replayTick - static_cast<long double>(mSpeedSpans.front().startTick));
    }
    if (replayTick >= static_cast<long double>(mSpeedSpans.back().endTick)) {
        return mSpeedSpanEndU + (replayTick - static_cast<long double>(mSpeedSpans.back().endTick));
    }
    auto const upper = std::upper_bound(
        mSpeedSpans.begin(),
        mSpeedSpans.end(),
        replayTick,
        [](long double value, SpeedSpan const& span) { return value < static_cast<long double>(span.endTick); }
    );
    auto const& span = (upper == mSpeedSpans.end()) ? mSpeedSpans.back() : *upper;
    return span.startUnscaled + (replayTick - static_cast<long double>(span.startTick)) / span.speed;
}

long double CameraTimelineEvaluator::replayTickForUnscaled(long double unscaledTicks) const noexcept {
    if (mSpeedSpans.empty()) return unscaledTicks;
    if (unscaledTicks <= mSpeedSpans.front().startUnscaled) {
        return static_cast<long double>(mSpeedSpans.front().startTick)
             + (unscaledTicks - mSpeedSpans.front().startUnscaled);
    }
    if (unscaledTicks >= mSpeedSpanEndU) {
        return static_cast<long double>(mSpeedSpans.back().endTick) + (unscaledTicks - mSpeedSpanEndU);
    }
    auto const upper = std::upper_bound(
        mSpeedSpans.begin(),
        mSpeedSpans.end(),
        unscaledTicks,
        [](long double value, SpeedSpan const& span) { return value < span.startUnscaled; }
    );
    auto const& span = (upper == mSpeedSpans.begin()) ? mSpeedSpans.front() : *std::prev(upper);
    return static_cast<long double>(span.startTick) + (unscaledTicks - span.startUnscaled) * span.speed;
}

long double CameraTimelineEvaluator::mapSegmentSampleTick(
    CameraTrackSegment const& segment,
    long double               replayTick
) const noexcept {
    if (!segment.hasUnscaledIntervals || segment.intervals.empty()) return replayTick;
    if (replayTick < segment.intervals.front().rStart) {
        return static_cast<long double>(segment.firstTick) + (replayTick - segment.intervals.front().rStart);
    }
    for (auto const& iv : segment.intervals) {
        if (replayTick <= iv.rEnd) {
            long double const raw =
                iv.ignoreClipSpeed ? static_cast<long double>(iv.kStartTick) + (unscaledTicksAt(replayTick) - iv.uStart)
                                   : static_cast<long double>(iv.kStartTick) + (replayTick - iv.rStart);
            return std::clamp(raw, static_cast<long double>(iv.kStartTick), static_cast<long double>(iv.kEndTick));
        }
    }
    return static_cast<long double>(segment.lastTick) + (replayTick - segment.intervals.back().rEnd);
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

    auto const mappedTick = mapSegmentSampleTick(*matched, tick);
    auto const keyTick    = clamp ? std::clamp(
                                     mappedTick,
                                     static_cast<long double>(matched->firstTick),
                                     static_cast<long double>(matched->lastTick)
                                 )
                                  : mappedTick;
    auto change = matched->track.createChange(keyTick);
    if (!change) return std::nullopt;

    CameraRenderStateHandler handler;
    change->apply(handler);
    auto state = handler.state();
    if (!state) return std::nullopt;
    applyRotationShake(*state, camera.shake.value_or(state::editing::model::CameraShake{}), mappedTick);
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
        if (!cameraCoversTick(camera, tick)) continue;
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
        if (!cameraCoversTick(camera, tick)) continue;
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
        if (!isUsableCamera(camera) || !cameraCoversTick(camera, tick)) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true, true)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
        }
    }
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
        if (!cameraCoversTick(camera, tick)) continue;
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
