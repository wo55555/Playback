#pragma once

#include "playback/editor/exporting/ExportTypes.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playback::editor {

enum class EditorActionType {
    TogglePause,
    Seek,
    SkipToStart,
    SkipToEnd,
    DecreaseSpeed,
    IncreaseSpeed,
    StopReplay,
    StartExport,
    CancelExport,
    OpenReplayBrowser,
    CloseReplayBrowser,
    RefreshReplayBrowser,
    OpenReplay,
    ImportReplay,
    DeleteReplays,
    RenameReplay,
    ShowReplayInFolder,
    ClearReplayBrowserError,
    UndoEditorEdit,
    RedoEditorEdit,
    AddFreeCamera,
    AddCameraSequence,
    DeleteCameraSequence,
    SplitSequence,
    TrimSequence,
    DeleteSequenceSegment,
    BindSequenceCamera,
    SplitWorldActor,
    TrimWorldActor,
    SetWorldActorSpeed,
    RippleDeleteWorldActorSegment,
    AddCameraKeyframe,
    MoveCameraKeyframe,
    DeleteCameraKeyframe,
    SetKeyframeInterpolation,
    SetCameraEnabled,
    DeleteCamera,
    UnbindCamera,
    CreateBindingCamera,
    SetSubActorDetails,
    SetPreviewCamera,
    ClearPreviewCamera,
};

struct EditorAction {
    EditorActionType                         type{};
    int                                      tick{};
    int                                      secondaryTick{};
    std::filesystem::path                    path;
    std::string                              replayId;
    std::string                              name;
    std::string                              id;
    std::string                              secondaryId;
    float                                    speed{};
    int                                      kind{};
    bool                                     value{};
    std::optional<exporting::ExportSettings> exportSettings;
    std::vector<std::string>                 replayIds;
    std::map<std::string, std::string>       details;
};

} // namespace playback::editor
