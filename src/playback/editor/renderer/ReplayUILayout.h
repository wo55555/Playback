#pragma once

#include <algorithm>

namespace playback::editor::ui {

struct ReplayUILayout {
    float scale{};
    float menuBarHeight{};
    float timelineHeight{};
    float gameViewportLeft{};
    float gameViewportTop{};
    float gameViewportRight{};
    float gameViewportBottom{};
};

[[nodiscard]] inline ReplayUILayout calculateReplayUILayout(float displayWidth, float displayHeight) {
    float const scale = std::clamp(displayHeight / 1080.0f, 0.6f, 1.5f);
    return {
        scale,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        displayWidth,
        displayHeight,
    };
}

} // namespace playback::editor::ui
