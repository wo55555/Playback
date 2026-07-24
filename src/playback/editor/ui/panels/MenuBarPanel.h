#pragma once

#include "playback/editor/context/EditorAction.h"
#include "playback/editor/context/EditorState.h"

#include <vector>

namespace playback::editor::ui {

struct ReplayUILayout;

void drawMenuBarPanel(EditorState const& state, ReplayUILayout const& layout, std::vector<EditorAction>& actions);

} // namespace playback::editor::ui
