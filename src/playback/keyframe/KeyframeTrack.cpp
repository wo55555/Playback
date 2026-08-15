#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <span>
#include <vector>

namespace playback::keyframe {

namespace {

using CameraInterpolationType      = state::editing::model::CameraInterpolationType;
using CameraSidedInterpolationType = state::editing::model::CameraSidedInterpolationType;
using CameraKeyframe               = state::editing::model::CameraKeyframe;
using Vec2                         = state::editing::model::Vec2;
using Vec3                         = state::editing::model::Vec3;

struct InterpolationSides {
    CameraSidedInterpolationType left;
    CameraSidedInterpolationType right;
};

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

Vec3 add(Vec3 const& left, Vec3 const& right) { return {left.x + right.x, left.y + right.y, left.z + right.z}; }

Vec3 scale(Vec3 const& value, float amount) { return {value.x * amount, value.y * amount, value.z * amount}; }

Vec3 lerp(Vec3 const& left, Vec3 const& right, float amount) {
    return add(left, scale({right.x - left.x, right.y - left.y, right.z - left.z}, amount));
}

float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float unwrapFrom(float reference, float value) { return reference + wrapDegrees(value - reference); }

float cubicBezier(float p0, float p1, float p2, float p3, float amount) {
    float const inverse = 1.0f - amount;
    return inverse * inverse * inverse * p0 + 3.0f * inverse * inverse * amount * p1
         + 3.0f * inverse * amount * amount * p2 + amount * amount * amount * p3;
}

float cubicBezierEase(float amount, Vec2 const& control1, Vec2 const& control2) {
    amount           = clampUnit(amount);
    float const x1   = clampUnit(control1.x);
    float const x2   = clampUnit(control2.x);
    float       low  = 0.0f;
    float       high = 1.0f;
    for (int iteration = 0; iteration < 18; ++iteration) {
        float const middle = (low + high) * 0.5f;
        if (cubicBezier(0.0f, x1, x2, 1.0f, middle) < amount) low = middle;
        else high = middle;
    }
    float const solved = (low + high) * 0.5f;
    return clampUnit(cubicBezier(0.0f, control1.y, control2.y, 1.0f, solved));
}

InterpolationSides sides(CameraInterpolationType type) {
    switch (type) {
    case CameraInterpolationType::Smooth:
        return {CameraSidedInterpolationType::Smooth, CameraSidedInterpolationType::Smooth};
    case CameraInterpolationType::EaseIn:
        return {CameraSidedInterpolationType::Ease, CameraSidedInterpolationType::Linear};
    case CameraInterpolationType::EaseOut:
        return {CameraSidedInterpolationType::Linear, CameraSidedInterpolationType::Ease};
    case CameraInterpolationType::EaseInOut:
        return {CameraSidedInterpolationType::Ease, CameraSidedInterpolationType::Ease};
    case CameraInterpolationType::Hold:
        return {CameraSidedInterpolationType::Hold, CameraSidedInterpolationType::Hold};
    case CameraInterpolationType::Hermite:
        return {CameraSidedInterpolationType::Hermite, CameraSidedInterpolationType::Hermite};
    case CameraInterpolationType::CubicBezier:
        return {CameraSidedInterpolationType::CubicBezier, CameraSidedInterpolationType::CubicBezier};
    case CameraInterpolationType::Linear:
    default:
        return {CameraSidedInterpolationType::Linear, CameraSidedInterpolationType::Linear};
    }
}

bool isCurve(CameraSidedInterpolationType type) {
    return type == CameraSidedInterpolationType::Smooth || type == CameraSidedInterpolationType::Hermite
        || type == CameraSidedInterpolationType::CubicBezier;
}

float interpolateSides(CameraSidedInterpolationType left, CameraSidedInterpolationType right, float amount) {
    if (isCurve(left)) left = isCurve(right) ? CameraSidedInterpolationType::Linear : right;
    if (isCurve(right)) right = left;
    if (left == CameraSidedInterpolationType::Hold) return 0.0f;
    if (right == CameraSidedInterpolationType::Hold) right = left;

    if (left == CameraSidedInterpolationType::Linear && right == CameraSidedInterpolationType::Linear) return amount;
    if (left == CameraSidedInterpolationType::Linear && right == CameraSidedInterpolationType::Ease) {
        return 1.0f - std::pow(1.0f - amount, 3.0f);
    }
    if (left == CameraSidedInterpolationType::Ease && right == CameraSidedInterpolationType::Linear) {
        return amount * amount * amount;
    }
    if (left == CameraSidedInterpolationType::Ease && right == CameraSidedInterpolationType::Ease) {
        return amount < 0.5f ? 4.0f * amount * amount * amount : 1.0f - std::pow(-2.0f * amount + 2.0f, 3.0f) * 0.5f;
    }
    return amount;
}

float adjustedInterval(float distance, float tickSpan, float relation) {
    if (tickSpan <= 0.0f || distance <= 0.0f || relation <= 0.0f) return 0.0f;
    float const factor = std::clamp(distance * relation / tickSpan, 0.4f, 2.5f);
    return distance * relation / factor;
}

float centripetalCatmullRom(
    float p0,
    float p1,
    float p2,
    float p3,
    float time1,
    float time2,
    float time3,
    float amount
) {
    auto const  distanceParameter = [](float left, float right) { return std::sqrt(std::abs(right - left)); };
    float const d1                = distanceParameter(p0, p1);
    float const d2                = distanceParameter(p1, p2);
    float const d3                = distanceParameter(p2, p3);
    float const average           = (d1 + d2 + d3) / 3.0f;
    if (average <= 0.0f || time3 <= 0.0f) return p1;

    float const relation = (time3 / 3.0f) / average;
    float const t0       = 0.0f;
    float const t1       = adjustedInterval(d1, time1, relation);
    float const t2       = t1 + adjustedInterval(d2, time2 - time1, relation);
    float const t3       = t2 + adjustedInterval(d3, time3 - time2, relation);
    float const t        = t1 + (t2 - t1) * amount;

    auto blend = [](float left, float right, float from, float to, float at) {
        float const value = from == to ? 0.5f : (at - from) / (to - from);
        return left + (right - left) * value;
    };
    float const a1 = blend(p0, p1, t0, t1, t);
    float const a2 = blend(p1, p2, t1, t2, t);
    float const a3 = blend(p2, p3, t2, t3, t);
    float const b1 = blend(a1, a2, t0, t2, t);
    float const b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

Vec3 centripetalCatmullRom(
    Vec3 const& p0,
    Vec3 const& p1,
    Vec3 const& p2,
    Vec3 const& p3,
    float       time1,
    float       time2,
    float       time3,
    float       amount
) {
    auto const distanceParameter = [](Vec3 const& left, Vec3 const& right) {
        float const dx = right.x - left.x;
        float const dy = right.y - left.y;
        float const dz = right.z - left.z;
        return std::sqrt(std::sqrt(dx * dx + dy * dy + dz * dz));
    };
    float const d1      = distanceParameter(p0, p1);
    float const d2      = distanceParameter(p1, p2);
    float const d3      = distanceParameter(p2, p3);
    float const average = (d1 + d2 + d3) / 3.0f;
    if (average <= 0.0f || time3 <= 0.0f) return p1;

    float const relation = (time3 / 3.0f) / average;
    float const t0       = 0.0f;
    float const t1       = adjustedInterval(d1, time1, relation);
    float const t2       = t1 + adjustedInterval(d2, time2 - time1, relation);
    float const t3       = t2 + adjustedInterval(d3, time3 - time2, relation);
    float const t        = t1 + (t2 - t1) * clampUnit(amount);

    auto blend = [](Vec3 const& left, Vec3 const& right, float from, float to, float at) {
        float const value = from == to ? 0.5f : (at - from) / (to - from);
        return lerp(left, right, value);
    };
    Vec3 const a1 = blend(p0, p1, t0, t1, t);
    Vec3 const a2 = blend(p1, p2, t1, t2, t);
    Vec3 const a3 = blend(p2, p3, t2, t3, t);
    Vec3 const b1 = blend(a1, a2, t0, t2, t);
    Vec3 const b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

float interpolatePolynomial(std::span<int const> ticks, std::span<float const> values, long double tick) {
    std::vector<long double> coefficients;
    coefficients.reserve(values.size());
    for (auto value : values) coefficients.push_back(value);

    for (size_t order = 1; order < coefficients.size(); ++order) {
        for (size_t index = coefficients.size() - 1; index >= order; --index) {
            long double const denominator = static_cast<long double>(ticks[index] - ticks[index - order]);
            coefficients[index] =
                denominator == 0.0L ? 0.0L : (coefficients[index] - coefficients[index - 1]) / denominator;
        }
    }

    long double result = coefficients.back();
    for (size_t index = coefficients.size() - 1; index-- > 0;) {
        result = result * (tick - ticks[index]) + coefficients[index];
    }
    return static_cast<float>(result);
}

} // namespace

KeyframeTrack::KeyframeTrack(KeyMap const& keyframes) : mKeyframes(keyframes.begin(), keyframes.end()) {}

bool KeyframeTrack::empty() const noexcept { return mKeyframes.empty(); }

CameraKeyframeChange KeyframeTrack::changeFromKeyframe(CameraKeyframe const& key) const {
    return {key.position, key.yaw, key.pitch, key.roll, key.fov};
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

    auto angle = [&](float p0, float p1, float p2, float p3) {
        p0 = unwrapFrom(p1, p0);
        p2 = unwrapFrom(p1, p2);
        p3 = unwrapFrom(p2, p3);
        return wrapDegrees(centripetalCatmullRom(p0, p1, p2, p3, time1, time2, time3, amount));
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
        angle(beforeKey.yaw, leftKey.yaw, rightKey.yaw, afterKey.yaw),
        angle(beforeKey.pitch, leftKey.pitch, rightKey.pitch, afterKey.pitch),
        angle(beforeKey.roll, leftKey.roll, rightKey.roll, afterKey.roll),
        centripetalCatmullRom(beforeKey.fov, leftKey.fov, rightKey.fov, afterKey.fov, time1, time2, time3, amount),
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
    ticks.reserve(static_cast<size_t>(std::distance(runStart, runEnd)));
    for (auto it = runStart; it != runEnd; ++it) {
        auto const& keyframe = it->second;
        ticks.push_back(it->first);
        x.push_back(keyframe.position.x);
        y.push_back(keyframe.position.y);
        z.push_back(keyframe.position.z);
        yaw.push_back(yaw.empty() ? keyframe.yaw : unwrapFrom(yaw.back(), keyframe.yaw));
        pitch.push_back(pitch.empty() ? keyframe.pitch : unwrapFrom(pitch.back(), keyframe.pitch));
        roll.push_back(roll.empty() ? keyframe.roll : unwrapFrom(roll.back(), keyframe.roll));
        fov.push_back(keyframe.fov);
    }

    Vec3 const position{
        interpolatePolynomial(ticks, x, tick),
        interpolatePolynomial(ticks, y, tick),
        interpolatePolynomial(ticks, z, tick),
    };
    return {
        position,
        wrapDegrees(interpolatePolynomial(ticks, yaw, tick)),
        wrapDegrees(interpolatePolynomial(ticks, pitch, tick)),
        wrapDegrees(interpolatePolynomial(ticks, roll, tick)),
        interpolatePolynomial(ticks, fov, tick),
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
