#pragma once

#include "CameraRenderState.h"
#include "playback/state/editing/models/MathTypes.h"

namespace playback::keyframe {

class CameraKeyframeHandler;

struct CameraKeyframeChange {
    state::editing::model::Vec3 position{};
    float                       yaw{};
    float                       pitch{};
    float                       roll{};
    float                       fov{70.0f};
    float                       mDistance{0.0f};

    void apply(CameraKeyframeHandler& handler) const;

    [[nodiscard]] CameraRenderState toRenderState() const noexcept;
    [[nodiscard]] float             distance() const noexcept { return mDistance; }

    [[nodiscard]] static CameraKeyframeChange
    interpolate(CameraKeyframeChange const& left, CameraKeyframeChange const& right, float amount);
};

} // namespace playback::keyframe
