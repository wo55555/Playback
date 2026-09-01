#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/state/ReplayBrowserState.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <memory>
#include <string>

namespace playback::state {

struct EditorCapabilities {
    bool cameraEditing{};
    bool videoEditing{};
    bool videoExport{};
    bool ffmpegVideoExport{};
};

struct ProjectPersistenceState {
    bool        dirty{};
    std::string projectFile;
    std::string error;
};

struct EditorState {
    bool                                                         replayVisible{};
    bool                                                         editorVisible{};
    bool                                                         hudVisible{};
    bool                                                         paused{};
    float                                                        playbackSpeed{1.0f};
    int                                                          currentTick{};
    int                                                          totalTicks{};
    bool                                                         canUndo{};
    bool                                                         canRedo{};
    std::shared_ptr<state::editing::model::EditorStateExt const> project;
    EditorCapabilities                                           capabilities;
    ProjectPersistenceState                                      persistence;
    exporting::ExportStatus                                      exportStatus;
    ReplayBrowserState                                           browser;
};

} // namespace playback::state
