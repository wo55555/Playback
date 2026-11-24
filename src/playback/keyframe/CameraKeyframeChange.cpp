#include "CameraKeyframeChange.h"

#include "CameraKeyframeHandler.h"

#include <algorithm>

namespace playback::keyframe {

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
    return {position.x, position.y, position.z, yaw, pitch, roll, fov};
}

CameraKeyframeChange
CameraKeyframeChange::interpolate(CameraKeyframeChange const& left, CameraKeyframeChange const& right, float amount) {
    amount                = std::clamp(amount, 0.0f, 1.0f);
    bool const wrapAngles = left.mWrapAngles && right.mWrapAngles;
    return {
        {
         left.position.x + (right.position.x - left.position.x) * amount,
         left.position.y + (right.position.y - left.position.y) * amount,
         left.position.z + (right.position.z - left.position.z) * amount,
         },
        wrapAngles ? interpolateAngle(left.yaw, right.yaw, amount) : left.yaw + (right.yaw - left.yaw) * amount,
        wrapAngles ? interpolateAngle(left.pitch, right.pitch, amount)
                   : left.pitch + (right.pitch - left.pitch) * amount,
        interpolateAngle(left.roll, right.roll, amount),
        left.fov + (right.fov - left.fov) * amount,
        left.mDistance + (right.mDistance - left.mDistance) * amount,
        wrapAngles,
    };
}

} // namespace playback::keyframe
