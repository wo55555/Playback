#pragma once

#include "MathTypes.h"

#include <string>

namespace playback::refactor::editor {

struct WorldActorSegment {
    std::string id;
    int startTick{};
    int endTick{};
    int sourceTick{};
    float speed{1.0f};
    Color4 color{0.95f, 0.55f, 0.20f, 1.0f};
    bool locked{};
};

} // namespace playback::refactor::editor
