#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/state/ReplayBrowserState.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace playback::state {

enum class EditorHistoryEventKind : uint8_t {
    None,
    Undo,
    Redo,
};

struct EditorHistoryEvent {
    std::uint64_t          seq{};
    EditorHistoryEventKind kind{EditorHistoryEventKind::None};
    std::string            label;
    size_t                 steps{1};
};

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

struct ReplayPlayerView {
    std::string uuid;
    std::string name;
    bool        recorder{};
    bool        present{};
};

struct ReplayEntityView {
    std::string choice;
    std::string label;
    float       distance{};
    bool        mob{};
};

struct EditorState {
    bool                                                         replayVisible{};
    bool                                                         editorVisible{};
    bool                                                         hudVisible{};
    bool                                                         paused{};
    bool                                                         followingRecordedPlayer{};
    bool                                                         followThirdPerson{};
    bool                                                         replayHasServerResourcePacks{};
    bool                                                         loadedWithServerResourcePacksDisabled{};
    bool                                                         disableServerResourcePacks{};
    std::string                                                  replaySubjectChoice;
    // The camera track under the playhead, if any.
    std::string                                                  activeCameraId;
    std::vector<ReplayPlayerView>                                replayPlayers;
    std::vector<ReplayEntityView>                                replayEntities;
    float                                                        playbackSpeed{1.0f};
    int                                                          currentTick{};
    int                                                          totalTicks{};
    bool                                                         canUndo{};
    bool                                                         canRedo{};
    size_t                                                       undoCount{};
    size_t                                                       redoCount{};
    std::string                                                  undoLabel;
    std::string                                                  redoLabel;
    std::vector<std::string>                                     undoHistory;
    std::vector<std::string>                                     redoHistory;
    EditorHistoryEvent                                           historyEvent;
    std::shared_ptr<state::editing::model::EditorStateExt const> project;
    keyframe::CameraTimelineHandle                               cameraTimeline;
    EditorCapabilities                                           capabilities;
    ProjectPersistenceState                                      persistence;
    exporting::ExportStatus                                      exportStatus;
    ReplayBrowserState                                           browser;
};

} // namespace playback::state
