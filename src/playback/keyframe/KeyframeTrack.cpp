#include "KeyframeTrack.h"

#include "InterpolationHelpers.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace playback::keyframe {

using namespace detail;

using CameraKeyframe = state::editing::model::CameraKeyframe;
using Vec3           = state::editing::model::Vec3;

KeyframeTrack::KeyframeTrack(KeyMap const& keyframes, bool wrapAngles)
: mKeyframes(keyframes.begin(), keyframes.end()),
  mWrapAngles(wrapAngles) {}

bool KeyframeTrack::empty() const noexcept { return mKeyframes.empty(); }

CameraKeyframeChange KeyframeTrack::changeFromKeyframe(CameraKeyframe const& key) const {
    return {key.position, key.yaw, key.pitch, key.roll, key.fov, key.thirdPersonDistance, mWrapAngles};
}

CameraKeyframeChange KeyframeTrack::smoothChange(KeyMapIter left, float amount) const {
    auto before = left;
    if (left != mKeyframes.begin()) before = std::prev(left);
    auto right = std::next(left);
    auto after = right;
    if (right != mKeyframes.end() && std::next(right) != mKeyframes.end()) after = std::next(right);
    if (before != left && before->second.interpolationType == CameraInterpolationType::Hold) before = left;
    if (after != right && right->second.interpolationType == CameraInterpolationType::Hold) after = right;

    auto const& beforeKey = before->second;
    auto const& leftKey   = left->second;
    auto const& rightKey  = right->second;
    auto const& afterKey  = after->second;
    float const time1     = static_cast<float>(left->first - before->first);
    float const time2     = static_cast<float>(right->first - before->first);
    float const time3     = static_cast<float>(after->first - before->first);

    auto wrappedAngle = [&](float p0, float p1, float p2, float p3) {
        p0 = unwrapFrom(p1, p0);
        p2 = unwrapFrom(p1, p2);
        p3 = unwrapFrom(p2, p3);
        return wrapDegrees(centripetalCatmullRom(p0, p1, p2, p3, time1, time2, time3, amount));
    };
    auto rawOrWrappedAngle = [&](float p0, float p1, float p2, float p3) {
        if (!mWrapAngles) return centripetalCatmullRom(p0, p1, p2, p3, time1, time2, time3, amount);
        return wrappedAngle(p0, p1, p2, p3);
    };

    return {
        centripetalCatmullRom(
            beforeKey.position,
            leftKey.position,
            rightKey.position,
            afterKey.position,
            time1,
            time2,
            time3,
            amount
        ),
        rawOrWrappedAngle(beforeKey.yaw, leftKey.yaw, rightKey.yaw, afterKey.yaw),
        rawOrWrappedAngle(beforeKey.pitch, leftKey.pitch, rightKey.pitch, afterKey.pitch),
        wrappedAngle(beforeKey.roll, leftKey.roll, rightKey.roll, afterKey.roll),
        centripetalCatmullRom(beforeKey.fov, leftKey.fov, rightKey.fov, afterKey.fov, time1, time2, time3, amount),
        centripetalCatmullRom(
            beforeKey.thirdPersonDistance,
            leftKey.thirdPersonDistance,
            rightKey.thirdPersonDistance,
            afterKey.thirdPersonDistance,
            time1,
            time2,
            time3,
            amount
        ),
        mWrapAngles,
    };
}

CameraKeyframeChange KeyframeTrack::hermiteChange(KeyMapIter left, long double tick) const {
    auto runStart = left;
    while (runStart != mKeyframes.begin()
           && std::prev(runStart)->second.interpolationType != CameraInterpolationType::Hold) {
        --runStart;
    }
    auto runEnd = left;
    ++runEnd;
    while (runEnd != mKeyframes.end() && runEnd->second.interpolationType != CameraInterpolationType::Hold) {
        ++runEnd;
    }

    std::vector<int>   ticks;
    std::vector<float> x;
    std::vector<float> y;
    std::vector<float> z;
    std::vector<float> yaw;
    std::vector<float> pitch;
    std::vector<float> roll;
    std::vector<float> fov;
    std::vector<float> distance;
    ticks.reserve(static_cast<size_t>(std::distance(runStart, runEnd)));
    for (auto it = runStart; it != runEnd; ++it) {
        auto const& keyframe = it->second;
        ticks.push_back(it->first);
        x.push_back(keyframe.position.x);
        y.push_back(keyframe.position.y);
        z.push_back(keyframe.position.z);
        yaw.push_back((!mWrapAngles || yaw.empty()) ? keyframe.yaw : unwrapFrom(yaw.back(), keyframe.yaw));
        pitch.push_back((!mWrapAngles || pitch.empty()) ? keyframe.pitch : unwrapFrom(pitch.back(), keyframe.pitch));
        roll.push_back(roll.empty() ? keyframe.roll : unwrapFrom(roll.back(), keyframe.roll));
        fov.push_back(keyframe.fov);
        distance.push_back(keyframe.thirdPersonDistance);
    }

    Vec3 const position{
        interpolatePolynomial(ticks, x, tick),
        interpolatePolynomial(ticks, y, tick),
        interpolatePolynomial(ticks, z, tick),
    };
    float const interpYaw   = interpolatePolynomial(ticks, yaw, tick);
    float const interpPitch = interpolatePolynomial(ticks, pitch, tick);
    return {
        position,
        mWrapAngles ? wrapDegrees(interpYaw) : interpYaw,
        mWrapAngles ? wrapDegrees(interpPitch) : interpPitch,
        wrapDegrees(interpolatePolynomial(ticks, roll, tick)),
        interpolatePolynomial(ticks, fov, tick),
        interpolatePolynomial(ticks, distance, tick),
        mWrapAngles,
    };
}

std::optional<CameraKeyframeChange> KeyframeTrack::createChange(long double tick) const {
    if (mKeyframes.empty() || !std::isfinite(tick)) return std::nullopt;
    auto const        front     = mKeyframes.begin();
    auto const        back      = std::prev(mKeyframes.end());
    long double const firstTick = static_cast<long double>(front->first);
    long double const lastTick  = static_cast<long double>(back->first);
    if (tick < firstTick || tick > lastTick) return std::nullopt;
    if (tick == firstTick) return changeFromKeyframe(front->second);
    if (tick == lastTick) return changeFromKeyframe(back->second);

    auto const right = mKeyframes.lower_bound(static_cast<int>(std::ceil(tick)));
    if (right == mKeyframes.end()) return changeFromKeyframe(back->second);
    if (static_cast<long double>(right->first) == tick) return changeFromKeyframe(right->second);

    auto const        left      = std::prev(right);
    auto const&       leftKey   = left->second;
    auto const&       rightKey  = right->second;
    long double const span      = static_cast<long double>(right->first - left->first);
    float const       amount    = span <= 0.0L ? 0.0f : clampUnit(static_cast<float>((tick - left->first) / span));
    auto const        leftSide  = sides(leftKey.interpolationType).right;
    auto              rightSide = sides(rightKey.interpolationType).left;

    if (leftSide == CameraSidedInterpolationType::Hold) return changeFromKeyframe(leftKey);
    if (rightSide == CameraSidedInterpolationType::Hold) rightSide = leftSide;

    auto regularChange = [&] {
        float adjusted = interpolateSides(leftSide, rightSide, amount);
        if (leftSide == CameraSidedInterpolationType::CubicBezier) {
            adjusted = cubicBezierEase(amount, leftKey.bezierCtrl1, leftKey.bezierCtrl2);
        } else if (rightSide == CameraSidedInterpolationType::CubicBezier) {
            adjusted = cubicBezierEase(amount, rightKey.bezierCtrl1, rightKey.bezierCtrl2);
        }
        return CameraKeyframeChange::interpolate(changeFromKeyframe(leftKey), changeFromKeyframe(rightKey), adjusted);
    };

    std::optional<CameraKeyframeChange> leftChange;
    std::optional<CameraKeyframeChange> rightChange;
    if (leftSide == CameraSidedInterpolationType::Smooth || rightSide == CameraSidedInterpolationType::Smooth) {
        auto smooth = smoothChange(left, amount);
        if (leftSide == CameraSidedInterpolationType::Smooth) leftChange = smooth;
        if (rightSide == CameraSidedInterpolationType::Smooth) rightChange = smooth;
    }
    if (leftSide == CameraSidedInterpolationType::Hermite || rightSide == CameraSidedInterpolationType::Hermite) {
        auto curve = hermiteChange(left, tick);
        if (leftSide == CameraSidedInterpolationType::Hermite) leftChange = curve;
        if (rightSide == CameraSidedInterpolationType::Hermite) rightChange = curve;
    }
    if (!leftChange) leftChange = regularChange();
    if (!rightChange) rightChange = regularChange();

    return CameraKeyframeChange::interpolate(*leftChange, *rightChange, amount);
}

} // namespace playback::keyframe
