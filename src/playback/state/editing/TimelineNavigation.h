#pragma once

#include <algorithm>

namespace playback::state::editing::TimelineNavigation {

// Resolves a smart 2-stage Home/End target:
// - toEnd == false (Home / SkipStart): stops at export In first when currentTick > inTick and inTick is an
//   active interior In point, otherwise jumps to 0.
// - toEnd == true (End / SkipEnd): stops at export Out first when currentTick < outTick and outTick is an
//   active interior Out point, otherwise jumps to totalTicks.
[[nodiscard]] inline int
resolveSmartEdgeTick(int currentTick, int totalTicks, int exportStartTick, int exportEndTick, bool toEnd) noexcept {
    int const total = std::max(0, totalTicks);
    if (total <= 0) return 0;
    int const cur = std::clamp(currentTick, 0, total);
    if (!toEnd) {
        bool const hasIn =
            exportStartTick > 0 && exportStartTick < total && (exportEndTick < 0 || exportStartTick < exportEndTick);
        if (hasIn && cur > exportStartTick) return exportStartTick;
        return 0;
    }
    bool const hasOut =
        exportEndTick > 0 && exportEndTick < total && (exportStartTick < 0 || exportEndTick > exportStartTick);
    if (hasOut && cur < exportEndTick) return exportEndTick;
    return total;
}

[[nodiscard]] inline int
resolveExportPointTick(int totalTicks, int exportStartTick, int exportEndTick, bool toOut) noexcept {
    int const total = std::max(0, totalTicks);
    if (total <= 0) return 0;
    if (!toOut) return exportStartTick >= 0 ? std::clamp(exportStartTick, 0, total) : 0;
    return exportEndTick >= 0 ? std::clamp(exportEndTick, 0, total) : total;
}

} // namespace playback::state::editing::TimelineNavigation
