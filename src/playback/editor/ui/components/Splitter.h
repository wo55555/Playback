#pragma once

#include "imgui.h"

namespace playback::editor::ui {

struct Rect {
    ImVec2 min;
    ImVec2 max;

    [[nodiscard]] float GetWidth() const { return max.x - min.x; }
    [[nodiscard]] float GetHeight() const { return max.y - min.y; }
};

class Splitter {
public:
    float drawVerticalSplit(float currentRatio, Rect area, float minRatio, float maxRatio);
    float drawHorizontalSplit(float currentRatio, Rect area, float minRatio, float maxRatio);
};

} // namespace playback::editor::ui
