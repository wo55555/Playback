#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/state/editing/models/MathTypes.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playback::state {

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
    SetKeyframePosition,
    SetKeyframeRotation,
    SetKeyframeFov,
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
    state::editing::model::Vec3              position{};
    std::optional<exporting::ExportSettings> exportSettings;
    std::vector<std::string>                 replayIds;
    std::map<std::string, std::string>       details;
};

} // namespace playback::state
