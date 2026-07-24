#pragma once

#include "playback/editor/context/EditorAction.h"
#include "playback/editor/context/EditorState.h"

#include "imgui.h"

#include <vector>

namespace playback::editor::ui {

struct ReplayUILayout;

void drawTimelinePanel(EditorState const& state, ReplayUILayout const& layout, std::vector<EditorAction>& actions);

void drawSkipControl(ImDrawList& drawList, struct ImVec2 center, float size, bool forwards, unsigned int color);

void drawRateControl(ImDrawList& drawList, struct ImVec2 center, float size, bool forwards, unsigned int color);

void drawPlaybackAction(ImDrawList& drawList, struct ImVec2 center, float size, bool paused, unsigned int color);

void drawCenteredFittedText(
    ImDrawList&  drawList,
    float        x,
    float        y,
    float        availableWidth,
    unsigned int color,
    char const*  text
);

struct TimelineScale {
    int  ticksPerMinor{};
    int  minorsPerMajor{};
    bool showSubSeconds{};
};

TimelineScale chooseTimelineScale(int totalTicks, float timelineWidth);

} // namespace playback::editor::ui
