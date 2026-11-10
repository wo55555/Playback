#pragma once

namespace playback::editor::graphics {

// Skips MinecraftGame::updateGraphics while a replay seek catches up, so the screen keeps its last frame.
[[nodiscard]] bool hookSeekFreeze(bool enable);

// True while the editor viewport should keep showing its last copied game frame; safe on the render thread.
[[nodiscard]] bool isSeekDisplayFrozen() noexcept;

} // namespace playback::editor::graphics
