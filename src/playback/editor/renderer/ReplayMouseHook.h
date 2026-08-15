#pragma once

class ClientInstance;

namespace playback::editor::renderer {

[[nodiscard]] bool hookReplayMouse(bool enable);

void setReplayMouseInputActive(bool active);
void setReplayUIActive(bool active);

void beginReplayMouseFrame(float displayWidth, float displayHeight, bool blockGameMouseInput);
void setReplayGameViewport(float left, float top, float right, float bottom);

void endReplayMouseFrame();

// Runs on the client update thread after ClientInstance::$update.
void updateReplayMouseOwnership(ClientInstance& client);

} // namespace playback::editor::renderer
