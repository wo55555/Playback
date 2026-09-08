#pragma once

#include <algorithm>

namespace playback::editor::ui {

[[nodiscard]] inline float calculateReplayUIScale(float displayHeight) {
    return std::clamp(displayHeight / 1080.0f, 0.6f, 1.5f);
}

} // namespace playback::editor::ui
