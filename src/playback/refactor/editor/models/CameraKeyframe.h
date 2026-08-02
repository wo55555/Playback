#pragma once

#include "MathTypes.h"

#include <string>

namespace playback::refactor::editor {

struct CameraKeyframe {
    std::string id;
    int         tick{};

    Vec3  position{0, 80, 0};
    float yaw{0.0f};
    float pitch{0.0f};
    float fov{90.0f};
    Color4 tint{1,1,1,1};

    // Easing type
    EasingType easingType{EasingType::Linear};
};

} // namespace playback::refactor::editor
