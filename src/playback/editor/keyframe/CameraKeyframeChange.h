#pragma once

#include "CameraRenderState.h"
#include "playback/editor/editing/models/MathTypes.h"

namespace playback::editor::keyframe {

class CameraKeyframeHandler;

struct CameraKeyframeChange {
    editing::model::Vec3 position{};
    float yaw{};
    float pitch{};
    float roll{};

    void apply(CameraKeyframeHandler& handler) const;

    [[nodiscard]] CameraRenderState toRenderState() const noexcept;

    [[nodiscard]] static CameraKeyframeChange

    interpolate(CameraKeyframeChange const& left, CameraKeyframeChange const& right, float amount);
};

} // namespace playback::editor::keyframe
