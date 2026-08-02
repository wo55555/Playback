#pragma once

#include "imgui.h"

namespace playback::editor::ui {

struct Rect {
    ImVec2 min;
    ImVec2 max;

    [[nodiscard]] float GetWidth() const { return max.x - min.x; }
    [[nodiscard]] float GetHeight() const { return max.y - min.y; }
    [[nodiscard]] bool contains(const ImVec2& point) const {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }
};

class Splitter {
public:
    float drawVerticalSplit(float currentRatio, Rect area, float minRatio, float maxRatio);
    float drawHorizontalSplit(float currentRatio, Rect area, float minRatio, float maxRatio);
};

} // namespace playback::editor::ui
