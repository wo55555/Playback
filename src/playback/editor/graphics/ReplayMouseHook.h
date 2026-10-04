#pragma once

class ClientInstance;

namespace playback::editor::graphics {

[[nodiscard]] bool hookReplayMouse(bool enable);

void setReplayMouseInputActive(bool active);
void setReplayUIActive(bool active);

void beginReplayMouseFrame(float displayWidth, float displayHeight, bool blockGameMouseInput);
void setReplayGameViewport(float left, float top, float right, float bottom);
// UI drawn over the game viewport (floating transport); clicks inside stay with ImGui.
void setReplayGameViewportExclusion(float left, float top, float right, float bottom);

// `window` is the game HWND; the ImGui cursor shape is applied on its thread.
void endReplayMouseFrame(void* window);

// Runs on the client update thread after ClientInstance::$update.
void updateReplayMouseOwnership(ClientInstance& client);

} // namespace playback::editor::graphics
