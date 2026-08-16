#pragma once

#include "playback/state/editing/models/CameraKeyframe.h"
#include "playback/state/editing/models/MathTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace playback::keyframe::detail {

using CameraInterpolationType      = state::editing::model::CameraInterpolationType;
using CameraSidedInterpolationType = state::editing::model::CameraSidedInterpolationType;
using Vec2                         = state::editing::model::Vec2;
using Vec3                         = state::editing::model::Vec3;

inline float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

inline Vec3 add(Vec3 const& left, Vec3 const& right) { return {left.x + right.x, left.y + right.y, left.z + right.z}; }

inline Vec3 scale(Vec3 const& value, float amount) { return {value.x * amount, value.y * amount, value.z * amount}; }

inline Vec3 lerp(Vec3 const& left, Vec3 const& right, float amount) {
    return add(left, scale({right.x - left.x, right.y - left.y, right.z - left.z}, amount));
}

inline float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

inline float unwrapFrom(float reference, float value) { return reference + wrapDegrees(value - reference); }

inline float cubicBezier(float p0, float p1, float p2, float p3, float amount) {
    float const inverse = 1.0f - amount;
    return inverse * inverse * inverse * p0 + 3.0f * inverse * inverse * amount * p1
         + 3.0f * inverse * amount * amount * p2 + amount * amount * amount * p3;
}

inline float cubicBezierEase(float amount, Vec2 const& control1, Vec2 const& control2) {
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

struct InterpolationSides {
    CameraSidedInterpolationType left;
    CameraSidedInterpolationType right;
};

inline InterpolationSides sides(CameraInterpolationType type) {
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

inline bool isCurve(CameraSidedInterpolationType type) {
    return type == CameraSidedInterpolationType::Smooth || type == CameraSidedInterpolationType::Hermite
        || type == CameraSidedInterpolationType::CubicBezier;
}

inline float interpolateSides(CameraSidedInterpolationType left, CameraSidedInterpolationType right, float amount) {
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

inline float adjustedInterval(float distance, float tickSpan, float relation) {
    if (tickSpan <= 0.0f || distance <= 0.0f || relation <= 0.0f) return 0.0f;
    float const factor = std::clamp(distance * relation / tickSpan, 0.4f, 2.5f);
    return distance * relation / factor;
}

inline float centripetalCatmullRom(
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

inline Vec3 centripetalCatmullRom(
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

inline float interpolatePolynomial(std::span<int const> ticks, std::span<float const> values, long double tick) {
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

} // namespace playback::keyframe::detail
