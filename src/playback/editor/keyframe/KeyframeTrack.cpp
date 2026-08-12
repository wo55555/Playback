#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace playback::editor::keyframe {

namespace {

using Vec2 = editing::model::Vec2;
using Vec3 = editing::model::Vec3;

float clampUnit(float value) { return std::clamp(value, 0.0f, 1.0f); }

Vec3 add(Vec3 const& left, Vec3 const& right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 subtract(Vec3 const& left, Vec3 const& right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

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

float lerpAngle(float left, float right, float amount) {
    return wrapDegrees(left + wrapDegrees(right - left) * amount);
}

float cubicBezier(float p0, float p1, float p2, float p3, float amount) {
    float const inverse = 1.0f - amount;
    return inverse * inverse * inverse * p0 + 3.0f * inverse * inverse * amount * p1
         + 3.0f * inverse * amount * amount * p2 + amount * amount * amount * p3;
}

Vec3 cubicBezier(Vec3 const& p0, Vec3 const& p1, Vec3 const& p2, Vec3 const& p3, float amount) {
    float const inverse = 1.0f - amount;
    float const a       = inverse * inverse * inverse;
    float const b       = 3.0f * inverse * inverse * amount;
    float const c       = 3.0f * inverse * amount * amount;
    float const d       = amount * amount * amount;
    return add(add(scale(p0, a), scale(p1, b)), add(scale(p2, c), scale(p3, d)));
}

float cubicBezierEase(float amount, Vec2 const& control1, Vec2 const& control2) {
    amount          = clampUnit(amount);
    float const x1  = clampUnit(control1.x);
    float const x2  = clampUnit(control2.x);
    float       low = 0.0f;
    float       high = 1.0f;
    for (int iteration = 0; iteration < 18; ++iteration) {
        float const middle = (low + high) * 0.5f;
        if (cubicBezier(0.0f, x1, x2, 1.0f, middle) < amount) low = middle;
        else high = middle;
    }
    float const solved = (low + high) * 0.5f;
    return clampUnit(cubicBezier(0.0f, control1.y, control2.y, 1.0f, solved));
}

float eased(float amount, editing::model::CameraKeyframe const& key) {
    amount = clampUnit(amount);
    switch (key.easingType) {
    case editing::model::EasingType::Hold: return 0.0f;
    case editing::model::EasingType::EaseIn: return amount * amount;
    case editing::model::EasingType::EaseOut: return 1.0f - (1.0f - amount) * (1.0f - amount);
    case editing::model::EasingType::EaseInOut:
        return amount < 0.5f ? 2.0f * amount * amount
                             : 1.0f - std::pow(-2.0f * amount + 2.0f, 2.0f) * 0.5f;
    case editing::model::EasingType::CubicBezier:
        return cubicBezierEase(amount, key.bezierCtrl1, key.bezierCtrl2);
    case editing::model::EasingType::Linear:
    default: return amount;
    }
}

Vec3 catmullRom(Vec3 const& p0, Vec3 const& p1, Vec3 const& p2, Vec3 const& p3, float amount) {
    auto const control1 = add(p1, scale(subtract(p2, p0), 1.0f / 6.0f));
    auto const control2 = subtract(p2, scale(subtract(p3, p1), 1.0f / 6.0f));
    return cubicBezier(p1, control1, control2, p2, amount);
}

Vec3 hermite(Vec3 const& p0, Vec3 const& tangent0, Vec3 const& p1, Vec3 const& tangent1, float amount) {
    float const t2  = amount * amount;
    float const t3  = t2 * amount;
    float const h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float const h10 = t3 - 2.0f * t2 + amount;
    float const h01 = -2.0f * t3 + 3.0f * t2;
    float const h11 = t3 - t2;
    return add(add(scale(p0, h00), scale(tangent0, h10)), add(scale(p1, h01), scale(tangent1, h11)));
}

} // namespace

KeyframeTrack::KeyframeTrack(std::span<editing::model::CameraKeyframe const> keyframes)
: mKeyframes(keyframes.begin(), keyframes.end()) {
    std::ranges::stable_sort(mKeyframes, {}, &editing::model::CameraKeyframe::tick);
}

bool KeyframeTrack::empty() const noexcept { return mKeyframes.empty(); }

CameraRenderState KeyframeTrack::stateFromKeyframe(editing::model::CameraKeyframe const& key) const {
    return {
        key.position.x,
        key.position.y,
        key.position.z,
        key.yaw,
        key.pitch,
        key.roll,
        std::clamp(key.fov, 1.0f, 179.0f),
    };
}

std::optional<CameraRenderState> KeyframeTrack::sample(long double tick) const {
    if (mKeyframes.empty() || !std::isfinite(tick)) return std::nullopt;
    if (tick <= mKeyframes.front().tick) return stateFromKeyframe(mKeyframes.front());
    if (tick >= mKeyframes.back().tick) return stateFromKeyframe(mKeyframes.back());

    auto const right = std::ranges::lower_bound(mKeyframes, tick, {}, &editing::model::CameraKeyframe::tick);
    if (right == mKeyframes.end()) return stateFromKeyframe(mKeyframes.back());
    if (static_cast<long double>(right->tick) == tick) return stateFromKeyframe(*right);
    if (right == mKeyframes.begin()) return stateFromKeyframe(*right);

    auto const& left = *(right - 1);
    auto const span  = static_cast<long double>(right->tick - left.tick);
    float const rawAmount = span <= 0.0L ? 0.0f : clampUnit(static_cast<float>((tick - left.tick) / span));
    float const amount    = eased(rawAmount, left);

    Vec3 position;
    switch (left.outgoingMotion.pathType) {
    case editing::model::CameraPathType::CubicBezier:
        position = cubicBezier(
            left.position,
            add(left.position, left.outgoingMotion.outControl),
            add(right->position, left.outgoingMotion.inControl),
            right->position,
            amount
        );
        break;
    case editing::model::CameraPathType::AutoSmooth: {
        auto const& before = right - 1 == mKeyframes.begin() ? left : *(right - 2);
        auto const& after  = right + 1 == mKeyframes.end() ? *right : *(right + 1);
        position = catmullRom(before.position, left.position, right->position, after.position, amount);
        break;
    }
    case editing::model::CameraPathType::Hermite:
        position = hermite(
            left.position,
            left.outgoingMotion.outControl,
            right->position,
            left.outgoingMotion.inControl,
            amount
        );
        break;
    case editing::model::CameraPathType::Linear:
    default: position = lerp(left.position, right->position, amount); break;
    }

    return CameraRenderState{
        position.x,
        position.y,
        position.z,
        lerpAngle(left.yaw, right->yaw, amount),
        lerpAngle(left.pitch, right->pitch, amount),
        lerpAngle(left.roll, right->roll, amount),
        std::clamp(
            left.fov + (right->fov - left.fov) * amount
                + std::sin(3.14159265358979323846f * amount) * left.outgoingMotion.fovPeakOffset,
            1.0f,
            179.0f
        ),
    };
}

std::optional<KeyframeTrackRange> KeyframeTrack::surroundingRange(long double tick) const noexcept {
    if (mKeyframes.empty() || !std::isfinite(tick)) return std::nullopt;
    if (mKeyframes.size() == 1) return KeyframeTrackRange{mKeyframes.front().tick, mKeyframes.front().tick};

    auto right = std::ranges::upper_bound(mKeyframes, tick, {}, &editing::model::CameraKeyframe::tick);
    size_t rightIndex = right == mKeyframes.end() ? mKeyframes.size() - 1
                                                  : static_cast<size_t>(right - mKeyframes.begin());
    size_t leftIndex  = rightIndex == 0 ? 0 : rightIndex - 1;
    size_t startIndex = leftIndex == 0 ? 0 : leftIndex - 1;
    size_t endIndex   = std::min(mKeyframes.size() - 1, rightIndex + 1);
    return KeyframeTrackRange{mKeyframes[startIndex].tick, mKeyframes[endIndex].tick};
}

std::vector<CameraRenderState>
KeyframeTrack::sampleRange(long double startTick, long double endTick, size_t maxSamples) const {
    std::vector<CameraRenderState> result;
    if (mKeyframes.empty() || maxSamples == 0 || !std::isfinite(startTick) || !std::isfinite(endTick)
        || endTick < startTick) {
        return result;
    }

    if (startTick == endTick || maxSamples == 1) {
        if (auto state = sample(startTick)) result.push_back(*state);
        return result;
    }

    size_t const count = std::max<size_t>(2, maxSamples);
    result.reserve(count);
    long double const span = endTick - startTick;
    for (size_t index = 0; index < count; ++index) {
        long double const amount = static_cast<long double>(index) / static_cast<long double>(count - 1);
        if (auto state = sample(startTick + span * amount)) result.push_back(*state);
    }
    return result;
}

} // namespace playback::editor::keyframe
