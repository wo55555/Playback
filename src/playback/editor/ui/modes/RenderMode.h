#pragma once

#include "playback/editor/ui/PanelContext.h"

#include <chrono>
#include <cstdint>

namespace playback::editor::ui {

class RenderMode {
public:
    // Called on entering render mode so each export measures its own elapsed time and speed.
    void reset();
    void draw(PanelContext const& ctx);

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point mStartedAt{Clock::now()};
    Clock::time_point mFirstFrameAt{};
    uint64_t          mFirstFrameCount{};
    bool              mHasFirstFrame{false};
};

} // namespace playback::editor::ui
