#pragma once

#include "MathTypes.h"

#include <string>

namespace playback::refactor::editor {

struct SequenceSegment {
    std::string id;
    int startTick{};
    int endTick{};
    std::string cameraId;
    Color4 color{0.20f, 0.55f, 0.95f, 1.0f};
    bool locked{};
};

} // namespace playback::refactor::editor
