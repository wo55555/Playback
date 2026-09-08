#pragma once

#include <algorithm>
#include <cmath>

namespace playback::editor::ui {

// Maps timeline ticks to screen X and back. Constructed per frame from zoom and scroll.
struct TimelineScale {
    float canvasLeft{};
    float canvasWidth{};
    float pixelsPerTick{1.0f};
    float scrollX{};
    int   totalTicks{};

    [[nodiscard]] float xAt(float tick) const { return canvasLeft + tick * pixelsPerTick - scrollX; }

    [[nodiscard]] int tickAt(float screenX) const {
        return static_cast<int>((screenX - canvasLeft + scrollX) / pixelsPerTick);
    }

    [[nodiscard]] float tickAtExact(float screenX) const { return (screenX - canvasLeft + scrollX) / pixelsPerTick; }

    [[nodiscard]] float contentWidth() const {
        return std::max(canvasWidth, static_cast<float>(totalTicks) * pixelsPerTick);
    }

    [[nodiscard]] float maxScroll() const { return std::max(0.0f, contentWidth() - canvasWidth); }

    [[nodiscard]] int firstVisibleTick(int step) const {
        if (step <= 0) return 0;
        return std::max(0, static_cast<int>(std::floor(scrollX / pixelsPerTick / step)) * step);
    }
};

} // namespace playback::editor::ui
