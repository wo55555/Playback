#pragma once

#include "playback/editor/context/ReplayBrowserState.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/exporting/ExportTypes.h"

#include <memory>

namespace playback::editor {

struct EditorCapabilities {
    bool cameraEditing{};
    bool videoEditing{};
    bool videoExport{};
    bool ffmpegVideoExport{};
};

struct EditorState {
    bool                                                  replayVisible{};
    bool                                                  editorVisible{};
    bool                                                  hudVisible{};
    bool                                                  paused{};
    float                                                 playbackSpeed{1.0f};
    int                                                   currentTick{};
    int                                                   totalTicks{};
    bool                                                  canUndo{};
    bool                                                  canRedo{};
    std::shared_ptr<editing::model::EditorStateExt const> project;
    EditorCapabilities                                    capabilities;
    exporting::ExportStatus                               exportStatus;
    ReplayBrowserState                                    browser;
};

} // namespace playback::editor
