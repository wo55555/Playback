#pragma once

namespace playback::editor::ui {

struct ReplayUILayout;

} // namespace playback::editor::ui

namespace playback::editor::renderer {

[[nodiscard]] bool hookReplayMouse(bool enable);

void setReplayMouseInputActive(bool active);
void setReplayUIActive(bool active);

void beginReplayMouseFrame(ui::ReplayUILayout const& layout, float displayWidth, float displayHeight);

void endReplayMouseFrame();

} // namespace playback::editor::renderer
