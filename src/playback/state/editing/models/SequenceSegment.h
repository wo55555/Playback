#pragma once

#include "MathTypes.h"

#include <string>
#include <string_view>

namespace playback::state::editing::model {

// Reserved camera id a segment binds to for the recorded player's own view; no CameraEntity carries it.
inline constexpr std::string_view kFirstPersonCameraId = "__first_person__";

struct SequenceSegment {
    std::string id;
    int         startTick{};
    int         endTick{};
    std::string cameraId;
    Color4      color{0.20f, 0.55f, 0.95f, 1.0f};
    bool        locked{};
};

} // namespace playback::state::editing::model
