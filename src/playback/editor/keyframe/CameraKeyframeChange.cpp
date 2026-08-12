#include "CameraKeyframeChange.h"

#include "CameraKeyframeHandler.h"

#include <algorithm>

namespace playback::editor::keyframe {

namespace {

float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float interpolateAngle(float left, float right, float amount) {
    return wrapDegrees(left + wrapDegrees(right - left) * amount);
}

} // namespace

void CameraKeyframeChange::apply(CameraKeyframeHandler& handler) const { handler.applyCamera(*this); }

CameraRenderState CameraKeyframeChange::toRenderState() const noexcept {
    return {position.x, position.y, position.z, yaw, pitch, roll, std::clamp(fov, 1.0f, 179.0f)};
}

CameraKeyframeChange CameraKeyframeChange::interpolate(
    CameraKeyframeChange const& left,
    CameraKeyframeChange const& right,
    float                       amount
) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    return {
        {
            left.position.x + (right.position.x - left.position.x) * amount,
            left.position.y + (right.position.y - left.position.y) * amount,
            left.position.z + (right.position.z - left.position.z) * amount,
        },
        interpolateAngle(left.yaw, right.yaw, amount),
        interpolateAngle(left.pitch, right.pitch, amount),
        interpolateAngle(left.roll, right.roll, amount),
        left.fov + (right.fov - left.fov) * amount,
    };
}

} // namespace playback::editor::keyframe
