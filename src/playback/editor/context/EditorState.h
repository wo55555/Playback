#pragma once

#include "playback/editor/context/ReplayBrowserState.h"

namespace playback::editor {

struct EditorCapabilities {
    bool cameraEditing{};
    bool videoEditing{};
    bool videoExport{};
};

struct EditorState {
    bool               replayVisible{};
    bool               editorVisible{};
    bool               hudVisible{};
    bool               paused{};
    float              playbackSpeed{1.0f};
    int                currentTick{};
    int                totalTicks{};
    EditorCapabilities capabilities;
    ReplayBrowserState browser;
};

} // namespace playback::editor
