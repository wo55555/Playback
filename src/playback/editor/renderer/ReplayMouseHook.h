#pragma once

namespace playback::editor::renderer {

[[nodiscard]] bool hookReplayMouse(bool enable);
[[nodiscard]] bool hookRecordingKeyboard(bool enable);

void setReplayMouseInputActive(bool active);
void setReplayUIActive(bool active);

void beginReplayMouseFrame(
    float displayWidth,
    float displayHeight,
    bool  blockGameMouseInput
);
void setReplayGameViewport(float left, float top, float right, float bottom);

void endReplayMouseFrame();

} // namespace playback::editor::renderer
