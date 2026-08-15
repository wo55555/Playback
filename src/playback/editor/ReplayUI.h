#pragma once

#include "playback/state/EditorAction.h"

namespace playback::editor {

[[nodiscard]] bool hookReplayUIRendererInit(bool enable);

[[nodiscard]] bool hookReplayUI(bool enable);

[[nodiscard]] bool isReplayBrowserVisible();

void tickReplayExportBeforeClientUpdate();
void tickReplayUI(bool hudVisible);

void submitEditorAction(state::EditorAction action);

} // namespace playback::editor
