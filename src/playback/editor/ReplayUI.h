#pragma once

namespace playback::editor {

[[nodiscard]] bool hookReplayUIRendererInit(bool enable);

[[nodiscard]] bool hookReplayUI(bool enable);

void tickReplayUI(bool hudVisible);

} // namespace playback::editor
