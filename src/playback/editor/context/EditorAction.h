#pragma once

#include <filesystem>
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
    DeleteCamera,
    UnbindCamera,
    SetCameraKind,
    CreateBindingCamera,
    SetSubActorDetails,
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
};

} // namespace playback::editor
