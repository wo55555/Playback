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
    constexpr float ReferenceHeight         = 1080.0f;
    constexpr float ReferenceMenuHeight     = 22.0f;
    constexpr float ReferenceTimelineHeight = 250.0f;

    float const scale                   = std::clamp(displayHeight / ReferenceHeight, 0.6f, 1.5f);
    float const menuBarHeight           = std::max(18.0f, ReferenceMenuHeight * scale);
    float const minimumGameHeight       = std::min(360.0f * scale, std::max(1.0f, displayHeight * 0.6f));
    float const availableTimelineHeight = std::max(0.0f, displayHeight - menuBarHeight - minimumGameHeight);
    float const timelineHeight          = std::min(ReferenceTimelineHeight * scale, availableTimelineHeight);
    float const gameViewportTop         = menuBarHeight;
    float const gameViewportBottom      = std::max(menuBarHeight, displayHeight - timelineHeight);
    float const availableGameHeight     = std::max(1.0f, gameViewportBottom - gameViewportTop);
    float const sourceAspect            = displayWidth / std::max(1.0f, displayHeight);
    float const gameViewportWidth       = std::min(displayWidth, availableGameHeight * sourceAspect);
    float const gameViewportLeft        = (displayWidth - gameViewportWidth) * 0.5f;

    ReplayUILayout layout;
    layout.scale              = scale;
    layout.menuBarHeight      = menuBarHeight;
    layout.timelineHeight     = timelineHeight;
    layout.gameViewportLeft   = gameViewportLeft;
    layout.gameViewportTop    = gameViewportTop;
    layout.gameViewportRight  = gameViewportLeft + gameViewportWidth;
    layout.gameViewportBottom = gameViewportBottom;
    return layout;
}

} // namespace playback::editor::ui
