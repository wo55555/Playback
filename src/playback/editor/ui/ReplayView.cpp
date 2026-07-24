#include "ReplayView.h"

#include "playback/editor/ui/panels/MenuBarPanel.h"
#include "playback/editor/ui/panels/TimelinePanel.h"

namespace playback::editor::ui {

void drawReplayView(EditorState const& state, ReplayUILayout const& layout, std::vector<EditorAction>& actions) {
    drawMenuBarPanel(state, layout, actions);
    drawTimelinePanel(state, layout, actions);
}

} // namespace playback::editor::ui
