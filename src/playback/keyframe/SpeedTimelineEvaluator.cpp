#include "SpeedTimelineEvaluator.h"

#include "InterpolationHelpers.h"
#include "TimelapseTimelineEvaluator.h"
#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <vector>

namespace playback::keyframe {

namespace {

using namespace detail;
using state::editing::model::MaxSpeedKeyframeValue;
using state::editing::model::MinSpeedKeyframeValue;
using state::editing::model::SpeedTrack;

[[nodiscard]] double clampSpeed(double value) noexcept {
    if (!std::isfinite(value)) return 1.0;
    return std::clamp(value, static_cast<double>(MinSpeedKeyframeValue), static_cast<double>(MaxSpeedKeyframeValue));
}

} // namespace

bool hasActiveSpeedCurve(state::editing::model::EditorStateExt const& project) noexcept {
    return project.speedTrack.enabled && !project.speedTrack.keysByTick.empty();
}

double sampleSpeedTrackAt(SpeedTrack const& track, long double replayTick) {
    if (!track.enabled || track.keysByTick.empty() || !std::isfinite(replayTick)) return 1.0;

    auto const&       keys      = track.keysByTick;
    auto const        front     = keys.begin();
    auto const        back      = std::prev(keys.end());
    long double const firstTick = static_cast<long double>(front->first);
    long double const lastTick  = static_cast<long double>(back->first);
    if (replayTick <= firstTick) return clampSpeed(static_cast<double>(front->second.speed));
    if (replayTick >= lastTick) return clampSpeed(static_cast<double>(back->second.speed));

    auto const right = keys.lower_bound(static_cast<int>(std::ceil(replayTick)));
    if (right == keys.end()) return clampSpeed(static_cast<double>(back->second.speed));
    if (static_cast<long double>(right->first) == replayTick) {
        return clampSpeed(static_cast<double>(right->second.speed));
    }

    auto const        left     = std::prev(right);
    auto const&       leftKey  = left->second;
    auto const&       rightKey = right->second;
    long double const span     = static_cast<long double>(right->first - left->first);
    float const       amount   = span <= 0.0L ? 0.0f : clampUnit(static_cast<float>((replayTick - left->first) / span));
    auto const        leftSide = sides(leftKey.interpolationType).right;
    auto              rightSide = sides(rightKey.interpolationType).left;

    if (leftSide == CameraSidedInterpolationType::Hold) return clampSpeed(static_cast<double>(leftKey.speed));
    if (rightSide == CameraSidedInterpolationType::Hold) rightSide = leftSide;

    auto regularValue = [&]() -> double {
        float adjusted = interpolateSides(leftSide, rightSide, amount);
        if (leftSide == CameraSidedInterpolationType::CubicBezier) {
            adjusted = cubicBezierEase(amount, leftKey.bezierCtrl1, leftKey.bezierCtrl2);
        } else if (rightSide == CameraSidedInterpolationType::CubicBezier) {
            adjusted = cubicBezierEase(amount, rightKey.bezierCtrl1, rightKey.bezierCtrl2);
        }
        double const a = clampSpeed(static_cast<double>(leftKey.speed));
        double const b = clampSpeed(static_cast<double>(rightKey.speed));
        return a + (b - a) * static_cast<double>(adjusted);
    };

    auto smoothValue = [&]() -> double {
        auto before = left;
        if (left != keys.begin()) before = std::prev(left);
        auto after = right;
        if (right != keys.end() && std::next(right) != keys.end()) after = std::next(right);
        if (before != left && before->second.interpolationType == CameraInterpolationType::Hold) before = left;
        if (after != right && right->second.interpolationType == CameraInterpolationType::Hold) after = right;

        float const p0    = static_cast<float>(clampSpeed(static_cast<double>(before->second.speed)));
        float const p1    = static_cast<float>(clampSpeed(static_cast<double>(leftKey.speed)));
        float const p2    = static_cast<float>(clampSpeed(static_cast<double>(rightKey.speed)));
        float const p3    = static_cast<float>(clampSpeed(static_cast<double>(after->second.speed)));
        float const time1 = static_cast<float>(left->first - before->first);
        float const time2 = static_cast<float>(right->first - before->first);
        float const time3 = static_cast<float>(after->first - before->first);
        return static_cast<double>(centripetalCatmullRom(p0, p1, p2, p3, time1, time2, time3, amount));
    };

    auto hermiteValue = [&]() -> double {
        auto runStart = left;
        while (runStart != keys.begin()
               && std::prev(runStart)->second.interpolationType != CameraInterpolationType::Hold) {
            --runStart;
        }
        auto runEnd = left;
        ++runEnd;
        while (runEnd != keys.end() && runEnd->second.interpolationType != CameraInterpolationType::Hold) {
            ++runEnd;
        }

        std::vector<int>   ticks;
        std::vector<float> values;
        size_t const       count = static_cast<size_t>(std::distance(runStart, runEnd));
        ticks.reserve(count);
        values.reserve(count);
        for (auto it = runStart; it != runEnd; ++it) {
            ticks.push_back(it->first);
            values.push_back(static_cast<float>(clampSpeed(static_cast<double>(it->second.speed))));
        }
        return static_cast<double>(interpolatePolynomial(ticks, values, replayTick));
    };

    std::optional<double> leftVal;
    std::optional<double> rightVal;
    if (leftSide == CameraSidedInterpolationType::Smooth || rightSide == CameraSidedInterpolationType::Smooth) {
        double const smooth = smoothValue();
        if (leftSide == CameraSidedInterpolationType::Smooth) leftVal = smooth;
        if (rightSide == CameraSidedInterpolationType::Smooth) rightVal = smooth;
    }
    if (leftSide == CameraSidedInterpolationType::Hermite || rightSide == CameraSidedInterpolationType::Hermite) {
        double const curve = hermiteValue();
        if (leftSide == CameraSidedInterpolationType::Hermite) leftVal = curve;
        if (rightSide == CameraSidedInterpolationType::Hermite) rightVal = curve;
    }
    if (!leftVal) leftVal = regularValue();
    if (!rightVal) rightVal = regularValue();

    double const blended = *leftVal + (*rightVal - *leftVal) * static_cast<double>(amount);
    return clampSpeed(blended);
}

double effectiveSpeedAt(state::editing::model::EditorStateExt const& project, long double replayTick) {
    if (!std::isfinite(replayTick)) return 1.0;
    if (auto const timelapseSpeed = sampleTimelapseSpeedAt(project.timelapseTrack, replayTick)) {
        return *timelapseSpeed;
    }
    return sampleSpeedTrackAt(project.speedTrack, replayTick);
}

SpeedTimeMapping::SpeedTimeMapping(state::editing::model::EditorStateExt const& project, int horizonTick)
: mTotalTicks(horizonTick >= 0 ? horizonTick : std::max(0, project.totalTicks)) {
    mUnscaledAtTick.reserve(static_cast<size_t>(mTotalTicks) + 1);
    mUnscaledAtTick.push_back(0.0);
    if (mTotalTicks <= 0) return;

    mSegmentSpeeds.reserve(static_cast<size_t>(mTotalTicks));
    long double accum = 0.0L;
    for (int k = 0; k < mTotalTicks; ++k) {
        double const spd = effectiveSpeedAt(project, static_cast<long double>(k) + 0.5L);
        mSegmentSpeeds.push_back(spd);
        accum += 1.0L / static_cast<long double>(spd);
        mUnscaledAtTick.push_back(static_cast<double>(accum));
    }
}

double SpeedTimeMapping::unscaledTicksAt(long double replayTick) const noexcept {
    if (!std::isfinite(replayTick)) return 0.0;
    if (mTotalTicks <= 0 || mUnscaledAtTick.size() < 2) return static_cast<double>(replayTick);
    if (replayTick <= 0.0L) return static_cast<double>(replayTick);
    if (replayTick >= static_cast<long double>(mTotalTicks)) {
        return mUnscaledAtTick.back() + static_cast<double>(replayTick - static_cast<long double>(mTotalTicks));
    }
    size_t const k =
        std::min<size_t>(static_cast<size_t>(std::floor(replayTick)), static_cast<size_t>(mTotalTicks - 1));
    double const frac = static_cast<double>(replayTick - static_cast<long double>(k));
    return mUnscaledAtTick[k] + frac / mSegmentSpeeds[k];
}

double SpeedTimeMapping::replayTickForUnscaled(long double unscaledTicks) const noexcept {
    if (!std::isfinite(unscaledTicks)) return 0.0;
    if (mTotalTicks <= 0 || mUnscaledAtTick.size() < 2) return static_cast<double>(unscaledTicks);
    if (unscaledTicks <= 0.0L) return static_cast<double>(unscaledTicks);
    if (unscaledTicks >= static_cast<long double>(mUnscaledAtTick.back())) {
        return static_cast<double>(mTotalTicks)
             + static_cast<double>(unscaledTicks - static_cast<long double>(mUnscaledAtTick.back()));
    }
    double const target = static_cast<double>(unscaledTicks);
    auto const   upper  = std::upper_bound(mUnscaledAtTick.begin(), mUnscaledAtTick.end(), target);
    size_t const k      = (upper == mUnscaledAtTick.begin())
                            ? 0
                            : std::min<size_t>(
                             static_cast<size_t>(std::distance(mUnscaledAtTick.begin(), upper) - 1),
                             static_cast<size_t>(mTotalTicks - 1)
                         );
    double const segU   = mUnscaledAtTick[k + 1] - mUnscaledAtTick[k];
    double const frac   = segU > 0.0 ? (target - mUnscaledAtTick[k]) / segU : 0.0;
    return static_cast<double>(k) + std::clamp(frac, 0.0, 1.0);
}

double SpeedTimeMapping::outputSecondsBetween(int startTick, int endTick) const noexcept {
    if (endTick <= startTick) return 0.0;
    return (unscaledTicksAt(static_cast<long double>(endTick)) - unscaledTicksAt(static_cast<long double>(startTick)))
         / 20.0;
}

double
SpeedTimeMapping::totalOutputSecondsForEnabledSlices(state::editing::model::EditorStateExt const& project) const {
    if (project.totalTicks <= 0) return 0.0;
    if (project.cameras.empty()) return outputSecondsBetween(0, project.totalTicks);

    auto const slices = state::editing::ClipOps::collectCompositedSlices(project, true);
    if (slices.empty()) return 0.0;

    bool const  variableTime = hasActiveSpeedCurve(project) || hasActiveTimelapseTrack(project);
    long double totalSeconds = 0.0L;
    for (auto const& slice : slices) {
        if (slice.inTick >= slice.outTick) continue;
        if (!variableTime) {
            totalSeconds += static_cast<long double>(slice.outTick - slice.inTick) / 20.0L;
            continue;
        }
        int const sStart = std::max(0, slice.inTick);
        int const sEnd   = std::max(sStart, slice.outTick);
        for (int k = sStart; k < sEnd; ++k) {
            double const effSpeed  = effectiveSpeedAt(project, static_cast<long double>(k) + 0.5L);
            totalSeconds          += 1.0L / (20.0L * static_cast<long double>(effSpeed));
        }
    }
    return static_cast<double>(totalSeconds);
}

} // namespace playback::keyframe
