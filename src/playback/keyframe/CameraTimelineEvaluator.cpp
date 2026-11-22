#include "CameraTimelineEvaluator.h"

#include "CameraFollowMath.h"
#include "CameraKeyframeHandler.h"
#include "InterpolationHelpers.h"
#include "KeyframeTrack.h"
#include "SpeedTimelineEvaluator.h"
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
    SubjectViewSampler                    subjectSampler,
    EntityPoseSampler                     entityPoseSampler,
    CameraRayClipper                      cameraRayClipper
)
: mProject(std::move(project)),
  mManualCameraId(std::move(manualCameraId)),
  mHoldOutsideTracks(holdOutsideTracks),
  mAnyCameraHasClips(std::ranges::any_of(mProject.cameras, [](auto const& c) { return !c.clips.empty(); })),
  mDimensionTransitionTicks(std::move(dimensionTransitionTicks)),
  mSubjectSampler(std::move(subjectSampler)),
  mEntityPoseSampler(std::move(entityPoseSampler)),
  mCameraRayClipper(std::move(cameraRayClipper)) {
    std::erase_if(mDimensionTransitionTicks, [this](int tick) { return tick < 0 || tick > mProject.totalTicks; });
    std::sort(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end());
    mDimensionTransitionTicks.erase(
        std::unique(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end()),
        mDimensionTransitionTicks.end()
    );
    buildSpeedSpans();

    auto buildTrackSegment = [this](size_t segmentIndex, KeyframeTrack::KeyMap const& segmentKeys) {
        int const                    firstTick = segmentKeys.begin()->first;
        int const                    lastTick  = segmentKeys.rbegin()->first;
        std::vector<SegmentInterval> intervals;
        bool                         hasUnscaledIntervals = false;

        if (segmentKeys.size() >= 2) {
            intervals.reserve(segmentKeys.size() - 1);
            long double rCursor = static_cast<long double>(firstTick);
            for (auto it = segmentKeys.begin(), next = std::next(it); next != segmentKeys.end(); it = next, ++next) {
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

        return CameraTrackSegment{
            segmentIndex,
            firstTick,
            lastTick,
            KeyframeTrack{segmentKeys},
            std::move(intervals),
            hasUnscaledIntervals,
        };
    };

    for (auto& camera : mProject.cameras) {
        if (camera.keysByTick.empty()) continue;

        std::vector<CameraTrackSegment> segments;
        KeyframeTrack::KeyMap           segmentKeys;
        size_t                          segmentIndex  = 0;
        auto                            appendSegment = [&] {
            if (segmentKeys.empty()) return;
            segments.emplace_back(buildTrackSegment(segmentIndex, segmentKeys));
            segmentKeys.clear();
        };

        std::vector<CustomThirdPersonTrackSegment> custom3pSegments;
        KeyframeTrack::KeyMap                      custom3pKeys;
        size_t                                     custom3pDimSegment    = 0;
        std::string                                custom3pSubject;
        bool                                       custom3pFollowRot     = false;
        bool                                       custom3pCollideBlocks = true;
        auto                                       appendCustom3pSegment = [&] {
            if (custom3pKeys.empty()) return;
            custom3pSegments.emplace_back(
                CustomThirdPersonTrackSegment{
                    buildTrackSegment(custom3pDimSegment, custom3pKeys),
                    custom3pSubject,
                    custom3pFollowRot,
                    custom3pCollideBlocks,
                }
            );
            custom3pKeys.clear();
        };

        // Split interpolated runs at dimension transitions and at person keyframes so splines never bridge them.
        // Split custom third-person runs when subject, follow-rotation frame, or dimension changes.
        for (auto const& [tick, keyframe] : camera.keysByTick) {
            auto const keySegment = dimensionSegmentForTick(tick);
            if (keyframe.mode == CameraKeyframeMode::Interpolated) {
                appendCustom3pSegment();
                if (segmentKeys.empty()) {
                    segmentIndex = keySegment;
                } else if (keySegment != segmentIndex) {
                    appendSegment();
                    segmentIndex = keySegment;
                }
                segmentKeys.emplace(tick, keyframe);
            } else if (keyframe.mode == CameraKeyframeMode::ThirdPerson && keyframe.thirdPersonCustom) {
                appendSegment();
                if (custom3pKeys.empty()) {
                    custom3pDimSegment    = keySegment;
                    custom3pSubject       = keyframe.subject;
                    custom3pFollowRot     = keyframe.thirdPersonFollowRotation;
                    custom3pCollideBlocks = keyframe.thirdPersonCollideBlocks;
                } else if (keySegment != custom3pDimSegment || keyframe.subject != custom3pSubject
                           || keyframe.thirdPersonFollowRotation != custom3pFollowRot) {
                    appendCustom3pSegment();
                    custom3pDimSegment    = keySegment;
                    custom3pSubject       = keyframe.subject;
                    custom3pFollowRot     = keyframe.thirdPersonFollowRotation;
                    custom3pCollideBlocks = keyframe.thirdPersonCollideBlocks;
                }
                CameraKeyframe proxy;
                proxy.position            = keyframe.thirdPersonOffset;
                proxy.yaw                 = keyframe.thirdPersonYaw;
                proxy.pitch               = keyframe.thirdPersonPitch;
                proxy.roll                = keyframe.thirdPersonRoll;
                proxy.fov                 = keyframe.thirdPersonFov;
                proxy.thirdPersonDistance = keyframe.thirdPersonDistance;
                proxy.interpolationType   = keyframe.interpolationType;
                proxy.bezierCtrl1         = keyframe.bezierCtrl1;
                proxy.bezierCtrl2         = keyframe.bezierCtrl2;
                proxy.ignoreClipSpeed     = keyframe.ignoreClipSpeed;
                custom3pKeys.emplace(tick, std::move(proxy));
            } else {
                appendSegment();
                appendCustom3pSegment();
            }
        }
        appendSegment();
        appendCustom3pSegment();
        if (!segments.empty()) mKeyframeTracks.emplace(camera.id, std::move(segments));
        if (!custom3pSegments.empty()) mCustomThirdPersonTracks.emplace(camera.id, std::move(custom3pSegments));
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

    if (!hasActiveSpeedCurve(mProject)) return;

    int horizonTick = std::max(0, mProject.totalTicks);
    for (auto const& camera : mProject.cameras) {
        if (!camera.keysByTick.empty()) {
            horizonTick = std::max(horizonTick, camera.keysByTick.rbegin()->first);
        }
        for (auto const& clip : camera.clips) {
            if (clip.inTick < clip.outTick) horizonTick = std::max(horizonTick, clip.outTick);
        }
    }
    if (horizonTick <= 0) return;

    long double cumU = 0.0L;
    for (int bStart = 0; bStart < horizonTick; ++bStart) {
        int const         bEnd  = bStart + 1;
        long double const mid   = static_cast<long double>(bStart) + 0.5L;
        long double const speed = static_cast<long double>(sampleSpeedTrackAt(mProject.speedTrack, mid));

        if (!mSpeedSpans.empty() && mSpeedSpans.back().endTick == bStart && mSpeedSpans.back().speed == speed) {
            mSpeedSpans.back().endTick = bEnd;
        } else {
            mSpeedSpans.push_back(SpeedSpan{bStart, bEnd, speed, cumU});
        }
        cumU += 1.0L / speed;
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
    CameraEntity const&              camera,
    long double                      tick,
    bool                             clamp,
    bool                             allowBeforeFirst,
    visuals::ReplaySampleTime const* sampleTimeForLookAt
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

    if (sampleTimeForLookAt && mEntityPoseSampler) {
        auto const firstIt = camera.keysByTick.find(matched->firstTick);
        auto const lastIt  = camera.keysByTick.find(matched->lastTick);
        if (firstIt != camera.keysByTick.end() && lastIt != camera.keysByTick.end()) {
            auto  leftIt  = firstIt;
            auto  rightIt = camera.keysByTick.end();
            float blendW  = 0.0f;

            if (keyTick <= static_cast<long double>(matched->firstTick)) {
                leftIt = firstIt;
            } else if (keyTick >= static_cast<long double>(matched->lastTick)) {
                leftIt = lastIt;
            } else {
                auto const upper = camera.keysByTick.lower_bound(static_cast<int>(std::ceil(keyTick)));
                if (upper == camera.keysByTick.end() || upper->first > matched->lastTick) {
                    leftIt = lastIt;
                } else if (static_cast<long double>(upper->first) == keyTick) {
                    leftIt = upper;
                } else if (upper != firstIt) {
                    leftIt  = std::prev(upper);
                    rightIt = upper;
                    long double const span = static_cast<long double>(rightIt->first - leftIt->first);
                    float const       amount =
                        span <= 0.0L ? 0.0f : clampUnit(static_cast<float>((keyTick - leftIt->first) / span));
                    auto const leftSide  = detail::sides(leftIt->second.interpolationType).right;
                    auto       rightSide = detail::sides(rightIt->second.interpolationType).left;
                    if (leftSide == state::editing::model::CameraSidedInterpolationType::Hold) {
                        blendW = 0.0f;
                    } else {
                        if (rightSide == state::editing::model::CameraSidedInterpolationType::Hold) rightSide = leftSide;
                        if (leftSide == state::editing::model::CameraSidedInterpolationType::CubicBezier) {
                            blendW =
                                detail::cubicBezierEase(amount, leftIt->second.bezierCtrl1, leftIt->second.bezierCtrl2);
                        } else if (rightSide == state::editing::model::CameraSidedInterpolationType::CubicBezier) {
                            blendW = detail::cubicBezierEase(
                                amount,
                                rightIt->second.bezierCtrl1,
                                rightIt->second.bezierCtrl2
                            );
                        } else if (detail::isCurve(leftSide) && detail::isCurve(rightSide)) {
                            blendW = amount * amount * (3.0f - 2.0f * amount);
                        } else {
                            blendW = detail::interpolateSides(leftSide, rightSide, amount);
                        }
                    }
                }
            }

            auto const& leftKey     = leftIt->second;
            bool const  hasRight    = rightIt != camera.keysByTick.end();
            bool const  leftLookAt  = leftKey.lookAtEnabled;
            bool const  rightLookAt = hasRight && rightIt->second.lookAtEnabled;
            if (leftLookAt || rightLookAt) {
                state::editing::model::Vec3 const camPos{state->x, state->y, state->z};
                auto sampleAim = [&](std::string const& target, float offsetY) -> std::optional<std::pair<float, float>> {
                    auto const pose = mEntityPoseSampler(*sampleTimeForLookAt, target);
                    if (!pose) return std::nullopt;
                    return aimAngles(camPos, {pose->x, pose->y + offsetY, pose->z});
                };

                if (leftLookAt && (!hasRight || (rightLookAt && leftKey.lookAtTarget == rightIt->second.lookAtTarget))) {
                    float const offsetY = hasRight ? std::lerp(leftKey.lookAtOffsetY, rightIt->second.lookAtOffsetY, blendW)
                                                   : leftKey.lookAtOffsetY;
                    if (auto const aim = sampleAim(leftKey.lookAtTarget, offsetY)) {
                        state->yaw   = aim->first;
                        state->pitch = aim->second;
                    }
                } else {
                    float leftYaw   = state->yaw;
                    float leftPitch = state->pitch;
                    if (leftLookAt) {
                        if (auto const aim = sampleAim(leftKey.lookAtTarget, leftKey.lookAtOffsetY)) {
                            leftYaw   = aim->first;
                            leftPitch = aim->second;
                        }
                    }
                    float rightYaw   = state->yaw;
                    float rightPitch = state->pitch;
                    if (rightLookAt) {
                        if (auto const aim = sampleAim(rightIt->second.lookAtTarget, rightIt->second.lookAtOffsetY)) {
                            rightYaw   = aim->first;
                            rightPitch = aim->second;
                        }
                    }
                    state->yaw   = wrapFollowDegrees(leftYaw + wrapFollowDegrees(rightYaw - leftYaw) * blendW);
                    state->pitch = std::lerp(leftPitch, rightPitch, blendW);
                }
            }
        }
    }

    applyRotationShake(*state, camera.shake.value_or(state::editing::model::CameraShake{}), mappedTick);
    return state;
}

std::optional<CameraRenderState> CameraTimelineEvaluator::sampleCustomThirdPerson(
    CameraEntity const&              camera,
    visuals::ReplaySampleTime const& time,
    bool                             clamp,
    bool                             allowBeforeFirst,
    CameraKeyframe const*            activeKey
) const {
    if (!mEntityPoseSampler) return std::nullopt;
    auto const tracks = mCustomThirdPersonTracks.find(camera.id);
    if (tracks == mCustomThirdPersonTracks.end() || tracks->second.empty()) return std::nullopt;

    long double const                    tick    = time.value();
    CustomThirdPersonTrackSegment const* matched = nullptr;
    for (auto const& entry : tracks->second) {
        if (static_cast<long double>(entry.segment.firstTick) <= tick) {
            matched = &entry;
        } else if (allowBeforeFirst && !matched) {
            matched = &entry;
            break;
        }
    }
    if (!matched) return std::nullopt;

    auto const mappedTick = mapSegmentSampleTick(matched->segment, tick);
    auto const keyTick    = clamp ? std::clamp(
                                     mappedTick,
                                     static_cast<long double>(matched->segment.firstTick),
                                     static_cast<long double>(matched->segment.lastTick)
                                 )
                                  : mappedTick;
    auto change = matched->segment.track.createChange(keyTick);
    if (!change) return std::nullopt;

    CameraRenderStateHandler handler;
    change->apply(handler);
    auto const params = handler.state();
    if (!params) return std::nullopt;

    std::string const& subject        = activeKey ? activeKey->subject : matched->subject;
    bool const         followRotation = activeKey ? activeKey->thirdPersonFollowRotation : matched->followRotation;
    bool const         collideBlocks  = activeKey ? activeKey->thirdPersonCollideBlocks : matched->collideBlocks;

    auto const subjectEye = mEntityPoseSampler(time, subject);
    if (!subjectEye) return std::nullopt;

    state::editing::model::Vec3 const offset{params->x, params->y, params->z};
    float const                       distance = std::max(0.0f, handler.distance());
    auto                              result   = computeCustomThirdPersonPose(
        *subjectEye,
        offset,
        distance,
        params->yaw,
        params->pitch,
        params->roll,
        params->fov,
        followRotation
    );
    if (collideBlocks && mCameraRayClipper && distance > 0.0f) {
        auto const clipped = mCameraRayClipper(result.anchor, {result.camera.x, result.camera.y, result.camera.z});
        result.camera.x    = clipped.x;
        result.camera.y    = clipped.y;
        result.camera.z    = clipped.z;
    }
    applyRotationShake(result.camera, camera.shake.value_or(state::editing::model::CameraShake{}), mappedTick);
    return result.camera;
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
        auto state = sampleKeyframes(camera, time.value(), true, false, &time);
        if (!state) return std::nullopt;
        return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
    }
    if (key->mode == CameraKeyframeMode::ThirdPerson && key->thirdPersonCustom) {
        auto state = sampleCustomThirdPerson(camera, time, true, false, key);
        if (!state) return std::nullopt;
        return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::ThirdPerson};
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
            return CameraShot{&camera, CameraKeyframeMode::Interpolated, {}, false};
        }
        // Person keyframes (including custom third-person) extend across dimension boundaries until the next keyframe.
        bool const customThirdPerson = key->mode == CameraKeyframeMode::ThirdPerson && key->thirdPersonCustom;
        return CameraShot{&camera, key->mode, key->subject, customThirdPerson};
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
        bool const customThirdPerson = key->mode == CameraKeyframeMode::ThirdPerson && key->thirdPersonCustom;
        // Without a sampler the caller views legacy person keyframes itself (the preview follows the subject natively).
        if (key->mode != CameraKeyframeMode::Interpolated && !customThirdPerson && !mSubjectSampler) {
            return std::nullopt;
        }
        // A track with nothing to show here (subject absent, etc.) yields to lower ones.
        if (auto evaluation = sampleTrack(camera, time)) return evaluation;
    }

    if (mManualCameraId && !mManualCameraId->empty()) {
        if (auto evaluation = sampleCameraById(*mManualCameraId, time)) return evaluation;
    }
    if (!mHoldOutsideTracks) return std::nullopt;
    for (auto const& camera : mProject.cameras) {
        if (!isUsableCamera(camera) || !cameraCoversTick(camera, tick)) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true, true, &time)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
        }
        if (auto state = sampleCustomThirdPerson(camera, time, true, true, nullptr)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::ThirdPerson};
        }
    }
    for (auto const& camera : mProject.cameras) {
        if (!isUsableCamera(camera)) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true, true, &time)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::Interpolated};
        }
        if (auto state = sampleCustomThirdPerson(camera, time, true, true, nullptr)) {
            return CameraTimelineEvaluation{*state, camera.id, CameraKeyframeMode::ThirdPerson};
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
        if (auto state = sampleKeyframes(camera, time.value(), true, false, nullptr)) {
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

    auto const [keyTick, key] = keyEntryAtOrBefore(*camera, time.floorTick());
    (void)keyTick;
    if (key && key->mode == CameraKeyframeMode::ThirdPerson && key->thirdPersonCustom) {
        if (auto state = sampleCustomThirdPerson(*camera, time, true, true, key)) {
            return CameraTimelineEvaluation{*state, camera->id, CameraKeyframeMode::ThirdPerson};
        }
    }
    if (auto state = sampleKeyframes(*camera, time.value(), true, true, &time)) {
        return CameraTimelineEvaluation{*state, camera->id, CameraKeyframeMode::Interpolated};
    }
    if (auto state = sampleCustomThirdPerson(*camera, time, true, true, nullptr)) {
        return CameraTimelineEvaluation{*state, camera->id, CameraKeyframeMode::ThirdPerson};
    }
    return std::nullopt;
}

} // namespace playback::keyframe
