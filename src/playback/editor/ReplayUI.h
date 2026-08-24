#pragma once

#include "playback/state/EditorAction.h"

namespace playback::editor {

[[nodiscard]] bool hookReplayUIRendererInit(bool enable);

[[nodiscard]] bool hookReplayUI(bool enable);

[[nodiscard]] bool isReplayBrowserVisible();

void tickReplayExportBeforeClientUpdate();
// Export waits resolve on the graphics path, so it also has to advance there instead of once per client tick.
void tickReplayExportDuringGraphics();
void tickReplayUI(bool hudVisible);

void submitEditorAction(state::EditorAction action);

} // namespace playback::editor
