#pragma once

#include <filesystem>
#include <map>
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
    SetKeyframeEasing,
    DeleteCamera,
    UnbindCamera,
    SetCameraKind,
    CreateBindingCamera,
    SetSubActorDetails,
    SetPreviewCamera,
    ClearPreviewCamera,
};

struct EditorAction {
    EditorActionType         type{};
    int                      tick{};
    std::filesystem::path    path;
    std::string              replayId;
    std::string              name;
    std::string              id;
    std::string              secondaryId;
    float                    speed{};
    int                      kind{};
    std::vector<std::string> replayIds;
    std::map<std::string, std::string> details;
};

} // namespace playback::editor
