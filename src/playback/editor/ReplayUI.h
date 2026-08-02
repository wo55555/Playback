#pragma once

#include "playback/editor/context/EditorAction.h"

namespace playback::editor {

[[nodiscard]] bool hookReplayUIRendererInit(bool enable);

[[nodiscard]] bool hookReplayUI(bool enable);

void tickReplayUI(bool hudVisible);

void submitEditorAction(EditorAction action);

} // namespace playback::editor
