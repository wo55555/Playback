#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/CameraKeyframe.h"
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
    DecreaseSpeed,
    IncreaseSpeed,
    SetPlaybackSpeed,
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
    SaveProject,
    LoadProject,
    TeleportToRecordedPlayer,
    SetFollowRecordedPlayer,
    SetReplaySubjectChoice,
    SetFollowThirdPerson,
    UndoEditorEdit,
    RedoEditorEdit,
    AddFreeCamera,
    SplitWorldActor,
    TrimWorldActor,
    RippleDeleteWorldActorSegment,
    AddCameraKeyframe,
    MoveCameraKeyframe,
    DeleteCameraKeyframe,
    SetKeyframeMode,
    SetKeyframeInterpolation,
    SetKeyframeBezierControls,
    SetKeyframePosition,
    SetKeyframeRotation,
    SetKeyframeFov,
    // A person keyframe's subject in secondaryId; empty is the recorder.
    SetKeyframeSubject,
    SetCameraEnabled,
    SetCameraLocked,
    SetCameraShake,
    RenameCamera,
    ClearCameraKeyframes,
    DeleteCamera,
    UnbindCamera,
    CreateBindingCamera,
    SetSubActorDetails,
    SetPreviewCamera,
    ClearPreviewCamera,
    MoveViewToKeyframe,
    MoveCameraKeyframes,
    DeleteCameraKeyframes,
    SetKeyframesMode,
    SetKeyframesInterpolation,
    AddCameraKeyframes,
    AddMarker,
    DeleteMarker,
    MoveMarker,
    RenameMarker,
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
    state::editing::model::Vec2              bezierCtrl1{};
    state::editing::model::Vec2              bezierCtrl2{};
    std::optional<exporting::ExportSettings> exportSettings;
    std::vector<std::string>                 replayIds;
    // Set only when pasting a copied keyframe. An empty value captures the live camera, as before.
    std::optional<editing::model::CameraKeyframe> keyframe;
    std::optional<editing::model::CameraShake>    shake;
    std::map<std::string, std::string>            details;
    // Batch keyframe edits (multi-select move / delete / interpolation / paste).
    std::vector<editing::model::KeyframeRef> keyframeRefs;
};

} // namespace playback::state
