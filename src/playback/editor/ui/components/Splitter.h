#pragma once

#include "imgui.h"

namespace playback::editor::ui {

struct Rect {
    ImVec2 min;
    ImVec2 max;

    [[nodiscard]] float GetWidth() const { return max.x - min.x; }
    [[nodiscard]] float GetHeight() const { return max.y - min.y; }
};

// Each split fills the current window, which must cover exactly the splitter band.
class Splitter {
public:
    // `currentRatio` is the right-hand share of `area`.
    float drawVerticalSplit(float currentRatio, Rect area, float minRatio, float maxRatio);
    // `currentRatio` is the top share of `area`.
    float drawHorizontalSplit(float currentRatio, Rect area, float minRatio, float maxRatio);

private:
    float mDragStartRatio{};
};

} // namespace playback::editor::ui
