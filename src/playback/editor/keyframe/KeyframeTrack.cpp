#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace playback::editor::keyframe {

namespace {

using CameraInterpolationType      = editing::model::CameraInterpolationType;
using CameraSidedInterpolationType = editing::model::CameraSidedInterpolationType;
using CameraKeyframe               = editing::model::CameraKeyframe;
using CameraPathType               = editing::model::CameraPathType;
using Vec2                         = editing::model::Vec2;
using Vec3                         = editing::model::Vec3;

struct InterpolationSides {
    CameraSidedInterpolationType left;
    CameraSidedInterpolationType right;
};

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

Vec3 add(Vec3 const& left, Vec3 const& right) { return {left.x + right.x, left.y + right.y, left.z + right.z}; }
Vec3 subtract(Vec3 const& left, Vec3 const& right) { return {left.x - right.x, left.y - right.y, left.z - right.z}; }
Vec3 scale(Vec3 const& value, float amount) { return {value.x * amount, value.y * amount, value.z * amount}; }

Vec3 lerp(Vec3 const& left, Vec3 const& right, float amount) {
    return {
        left.x + (right.x - left.x) * amount,
        left.y + (right.y - left.y) * amount,
        left.z + (right.z - left.z) * amount,
    };
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

Vec3 cubicBezier(Vec3 const& p0, Vec3 const& p1, Vec3 const& p2, Vec3 const& p3, float amount) {
    float const inverse = 1.0f - amount;
    return add(
        add(scale(p0, inverse * inverse * inverse), scale(p1, 3.0f * inverse * inverse * amount)),
        add(scale(p2, 3.0f * inverse * amount * amount), scale(p3, amount * amount * amount))
    );
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
        return amount < 0.5f ? 4.0f * amount * amount * amount
                             : 1.0f - std::pow(-2.0f * amount + 2.0f, 3.0f) * 0.5f;
    }
    return amount;
}

float hermite(float p0, float tangent0, float p1, float tangent1, float amount) {
    float const t2  = amount * amount;
    float const t3  = t2 * amount;
    float const h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float const h10 = t3 - 2.0f * t2 + amount;
    float const h01 = -2.0f * t3 + 3.0f * t2;
    float const h11 = t3 - t2;
    return p0 * h00 + tangent0 * h10 + p1 * h01 + tangent1 * h11;
}

Vec3 hermite(Vec3 const& p0, Vec3 const& tangent0, Vec3 const& p1, Vec3 const& tangent1, float amount) {
    return {
        hermite(p0.x, tangent0.x, p1.x, tangent1.x, amount),
        hermite(p0.y, tangent0.y, p1.y, tangent1.y, amount),
        hermite(p0.z, tangent0.z, p1.z, tangent1.z, amount),
    };
}

float distanceParameter(Vec3 const& left, Vec3 const& right) {
    auto const delta = subtract(right, left);
    return std::pow(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z, 0.25f);
}

float adjustedInterval(float distance, float tickSpan, float relation) {
    if (tickSpan <= 0.0f || distance <= 0.0f || relation <= 0.0f) return 0.0f;
    float const factor = std::clamp(distance * relation / tickSpan, 0.4f, 2.5f);
    return distance * relation / factor;
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
    float const t        = t1 + (t2 - t1) * amount;

    auto blend = [](Vec3 const& left, Vec3 const& right, float from, float to, float at) {
        return lerp(left, right, from == to ? 0.5f : (at - from) / (to - from));
    };
    auto const a1 = blend(p0, p1, t0, t1, t);
    auto const a2 = blend(p1, p2, t1, t2, t);
    auto const a3 = blend(p2, p3, t2, t3, t);
    auto const b1 = blend(a1, a2, t0, t2, t);
    auto const b2 = blend(a2, a3, t1, t3, t);
    return blend(b1, b2, t1, t2, t);
}

float interpolatePolynomial(
    std::span<CameraKeyframe const>                         keyframes,
    long double                                             tick,
    std::function<float(CameraKeyframe const&)> const& value
) {
    std::vector<long double> coefficients;
    coefficients.reserve(keyframes.size());
    for (auto const& keyframe : keyframes) coefficients.push_back(value(keyframe));

    for (size_t order = 1; order < coefficients.size(); ++order) {
        for (size_t index = coefficients.size() - 1; index >= order; --index) {
            long double const denominator =
                static_cast<long double>(keyframes[index].tick - keyframes[index - order].tick);
            coefficients[index] = denominator == 0.0L
                                    ? 0.0L
                                    : (coefficients[index] - coefficients[index - 1]) / denominator;
        }
    }

    long double result = coefficients.back();
    for (size_t index = coefficients.size() - 1; index-- > 0;) {
        result = result * (tick - keyframes[index].tick) + coefficients[index];
    }
    return static_cast<float>(result);
}

} // namespace

KeyframeTrack::KeyframeTrack(std::span<CameraKeyframe const> keyframes) : mKeyframes(keyframes.begin(), keyframes.end()) {
    std::ranges::stable_sort(mKeyframes, {}, &CameraKeyframe::tick);
}

bool KeyframeTrack::empty() const noexcept { return mKeyframes.empty(); }

CameraKeyframeChange KeyframeTrack::changeFromKeyframe(CameraKeyframe const& key) const {
    return {key.position, key.yaw, key.pitch, key.roll, std::clamp(key.fov, 1.0f, 179.0f)};
}

CameraKeyframeChange KeyframeTrack::smoothChange(size_t leftIndex, float amount) const {
    size_t beforeIndex = leftIndex == 0 ? leftIndex : leftIndex - 1;
    size_t rightIndex  = leftIndex + 1;
    size_t afterIndex  = std::min(rightIndex + 1, mKeyframes.size() - 1);
    if (beforeIndex != leftIndex && mKeyframes[beforeIndex].interpolationType == CameraInterpolationType::Hold) {
        beforeIndex = leftIndex;
    }
    if (afterIndex != rightIndex && mKeyframes[rightIndex].interpolationType == CameraInterpolationType::Hold) {
        afterIndex = rightIndex;
    }

    auto const& before = mKeyframes[beforeIndex];
    auto const& left   = mKeyframes[leftIndex];
    auto const& right  = mKeyframes[rightIndex];
    auto const& after  = mKeyframes[afterIndex];
    float const time1  = static_cast<float>(left.tick - before.tick);
    float const time2  = static_cast<float>(right.tick - before.tick);
    float const time3  = static_cast<float>(after.tick - before.tick);

    auto scalar = [&](float p0, float p1, float p2, float p3) {
        Vec3 const value = centripetalCatmullRom({p0, 0, 0}, {p1, 0, 0}, {p2, 0, 0}, {p3, 0, 0}, time1, time2, time3, amount);
        return value.x;
    };
    auto angle = [&](float p0, float p1, float p2, float p3) {
        p0 = unwrapFrom(p1, p0);
        p2 = unwrapFrom(p1, p2);
        p3 = unwrapFrom(p2, p3);
        return wrapDegrees(scalar(p0, p1, p2, p3));
    };

    return {
        centripetalCatmullRom(before.position, left.position, right.position, after.position, time1, time2, time3, amount),
        angle(before.yaw, left.yaw, right.yaw, after.yaw),
        angle(before.pitch, left.pitch, right.pitch, after.pitch),
        angle(before.roll, left.roll, right.roll, after.roll),
        scalar(before.fov, left.fov, right.fov, after.fov),
    };
}

CameraKeyframeChange KeyframeTrack::hermiteChange(size_t leftIndex, long double tick) const {
    size_t runStart = leftIndex;
    while (runStart > 0 && mKeyframes[runStart - 1].interpolationType != CameraInterpolationType::Hold) --runStart;
    size_t runEnd = leftIndex + 1;
    while (runEnd + 1 < mKeyframes.size() && mKeyframes[runEnd].interpolationType != CameraInterpolationType::Hold) {
        ++runEnd;
    }

    auto const keyframes = std::span<CameraKeyframe const>{mKeyframes}.subspan(runStart, runEnd - runStart + 1);
    std::vector<float> yaw;
    std::vector<float> pitch;
    std::vector<float> roll;
    yaw.reserve(keyframes.size());
    pitch.reserve(keyframes.size());
    roll.reserve(keyframes.size());
    for (auto const& keyframe : keyframes) {
        yaw.push_back(yaw.empty() ? keyframe.yaw : unwrapFrom(yaw.back(), keyframe.yaw));
        pitch.push_back(pitch.empty() ? keyframe.pitch : unwrapFrom(pitch.back(), keyframe.pitch));
        roll.push_back(roll.empty() ? keyframe.roll : unwrapFrom(roll.back(), keyframe.roll));
    }

    auto scalar = [&](std::function<float(CameraKeyframe const&)> const& value) {
        return interpolatePolynomial(keyframes, tick, value);
    };
    auto angle = [&](std::vector<float> const& values) {
        return wrapDegrees(interpolatePolynomial(keyframes, tick, [&](CameraKeyframe const& keyframe) {
            return values[static_cast<size_t>(&keyframe - keyframes.data())];
        }));
    };

    return {
        {
            scalar([](CameraKeyframe const& keyframe) { return keyframe.position.x; }),
            scalar([](CameraKeyframe const& keyframe) { return keyframe.position.y; }),
            scalar([](CameraKeyframe const& keyframe) { return keyframe.position.z; }),
        },
        angle(yaw),
        angle(pitch),
        angle(roll),
        scalar([](CameraKeyframe const& keyframe) { return keyframe.fov; }),
    };
}

Vec3 KeyframeTrack::samplePathPosition(size_t leftIndex, float amount) const {
    auto const& left  = mKeyframes[leftIndex];
    auto const& right = mKeyframes[leftIndex + 1];
    switch (left.outgoingMotion.pathType) {
    case CameraPathType::CubicBezier:
        return cubicBezier(
            left.position,
            add(left.position, left.outgoingMotion.outControl),
            add(right.position, left.outgoingMotion.inControl),
            right.position,
            amount
        );
    case CameraPathType::AutoSmooth: {
        auto const& before = leftIndex == 0 ? left : mKeyframes[leftIndex - 1];
        auto const& after  = leftIndex + 2 >= mKeyframes.size() ? right : mKeyframes[leftIndex + 2];
        return centripetalCatmullRom(
            before.position,
            left.position,
            right.position,
            after.position,
            static_cast<float>(left.tick - before.tick),
            static_cast<float>(right.tick - before.tick),
            static_cast<float>(after.tick - before.tick),
            amount
        );
    }
    case CameraPathType::Hermite:
        return hermite(
            left.position,
            left.outgoingMotion.outControl,
            right.position,
            left.outgoingMotion.inControl,
            amount
        );
    case CameraPathType::Linear:
    default:
        return lerp(left.position, right.position, amount);
    }
}

std::optional<CameraKeyframeChange> KeyframeTrack::createChange(long double tick) const {
    if (mKeyframes.empty() || !std::isfinite(tick)) return std::nullopt;
    if (tick <= mKeyframes.front().tick) return changeFromKeyframe(mKeyframes.front());
    if (tick >= mKeyframes.back().tick) return changeFromKeyframe(mKeyframes.back());

    auto const right = std::ranges::lower_bound(mKeyframes, tick, {}, &CameraKeyframe::tick);
    if (right == mKeyframes.end()) return changeFromKeyframe(mKeyframes.back());
    if (static_cast<long double>(right->tick) == tick) return changeFromKeyframe(*right);

    size_t const rightIndex = static_cast<size_t>(right - mKeyframes.begin());
    size_t const leftIndex  = rightIndex - 1;
    auto const& left        = mKeyframes[leftIndex];
    long double const span  = static_cast<long double>(right->tick - left.tick);
    float const amount      = span <= 0.0L ? 0.0f : clampUnit(static_cast<float>((tick - left.tick) / span));
    auto const leftSide     = sides(left.interpolationType).right;
    auto       rightSide    = sides(right->interpolationType).left;

    if (leftSide == CameraSidedInterpolationType::Hold) return changeFromKeyframe(left);
    if (rightSide == CameraSidedInterpolationType::Hold) rightSide = leftSide;

    auto regularChange = [&] {
        float adjusted = interpolateSides(leftSide, rightSide, amount);
        if (leftSide == CameraSidedInterpolationType::CubicBezier) {
            adjusted = cubicBezierEase(amount, left.bezierCtrl1, left.bezierCtrl2);
        } else if (rightSide == CameraSidedInterpolationType::CubicBezier) {
            adjusted = cubicBezierEase(amount, right->bezierCtrl1, right->bezierCtrl2);
        }
        return CameraKeyframeChange::interpolate(changeFromKeyframe(left), changeFromKeyframe(*right), adjusted);
    };

    std::optional<CameraKeyframeChange> leftChange;
    std::optional<CameraKeyframeChange> rightChange;
    if (leftSide == CameraSidedInterpolationType::Smooth || rightSide == CameraSidedInterpolationType::Smooth) {
        auto smooth = smoothChange(leftIndex, amount);
        if (leftSide == CameraSidedInterpolationType::Smooth) leftChange = smooth;
        if (rightSide == CameraSidedInterpolationType::Smooth) rightChange = smooth;
    }
    if (leftSide == CameraSidedInterpolationType::Hermite || rightSide == CameraSidedInterpolationType::Hermite) {
        auto curve = hermiteChange(leftIndex, tick);
        if (leftSide == CameraSidedInterpolationType::Hermite) leftChange = curve;
        if (rightSide == CameraSidedInterpolationType::Hermite) rightChange = curve;
    }
    if (!leftChange) leftChange = regularChange();
    if (!rightChange) rightChange = regularChange();

    auto result = CameraKeyframeChange::interpolate(*leftChange, *rightChange, amount);
    float pathAmount = interpolateSides(leftSide, rightSide, amount);
    if (leftSide == CameraSidedInterpolationType::CubicBezier) {
        pathAmount = cubicBezierEase(amount, left.bezierCtrl1, left.bezierCtrl2);
    } else if (rightSide == CameraSidedInterpolationType::CubicBezier) {
        pathAmount = cubicBezierEase(amount, right->bezierCtrl1, right->bezierCtrl2);
    }
    result.position = samplePathPosition(leftIndex, pathAmount);
    result.fov = std::clamp(
        result.fov + std::sin(3.14159265358979323846f * pathAmount) * left.outgoingMotion.fovPeakOffset,
        1.0f,
        179.0f
    );
    return result;
}

std::optional<KeyframeTrackRange> KeyframeTrack::surroundingRange(long double tick) const noexcept {
    if (mKeyframes.empty() || !std::isfinite(tick)) return std::nullopt;
    if (mKeyframes.size() == 1) return KeyframeTrackRange{mKeyframes.front().tick, mKeyframes.front().tick};

    auto const right = std::ranges::upper_bound(mKeyframes, tick, {}, &CameraKeyframe::tick);
    size_t const rightIndex =
        right == mKeyframes.end() ? mKeyframes.size() - 1 : static_cast<size_t>(right - mKeyframes.begin());
    size_t const leftIndex  = rightIndex == 0 ? 0 : rightIndex - 1;
    size_t const startIndex = leftIndex == 0 ? 0 : leftIndex - 1;
    size_t const endIndex   = std::min(mKeyframes.size() - 1, rightIndex + 1);
    return KeyframeTrackRange{mKeyframes[startIndex].tick, mKeyframes[endIndex].tick};
}

} // namespace playback::editor::keyframe
