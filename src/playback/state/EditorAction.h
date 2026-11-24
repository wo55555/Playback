#pragma once

#include "playback/exporting/ExportTypes.h"
#include "playback/state/editing/models/CameraEntity.h"
#include "playback/state/editing/models/CameraKeyframe.h"
#include "playback/state/editing/models/MathTypes.h"
#include "playback/state/editing/models/SelectionModel.h"

#include <cstddef>
#include <filesystem>
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
    // secondaryTick == 1 prefers the live preview subject (session.userReplaySubjectChoice()) over the previous
    // keyframe's subject when creating a 1P/3P keyframe.
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
    // id is the camera, kind its new index in the camera list (the list order is the shot priority).
    MoveCamera,
    ClearCameraKeyframes,
    DeleteCamera,
    SetPreviewCamera,
    ClearPreviewCamera,
    MoveViewToKeyframe,
    MoveCameraKeyframes,
    DeleteCameraKeyframes,
    SetKeyframesMode,
    SetKeyframesInterpolation,
    SetKeyframesSubject,
    AddCameraKeyframes,
    AddMarker,
    DeleteMarker,
    MoveMarker,
    RenameMarker,
    SetKeyframeIgnoreClipSpeed,
    SetKeyframesIgnoreClipSpeed,
    // id + tick + keyframe (carries updated thirdPerson* fields); value=true converts offset/yaw/pitch when
    // toggling thirdPersonFollowRotation to preserve the world camera view.
    SetKeyframeThirdPersonSettings,
    // keyframeRefs + value (new thirdPersonCustom flag for ThirdPerson keyframes).
    SetKeyframesThirdPersonCustom,
    // id + tick + value (lookAtEnabled) + secondaryId (lookAtTarget) + speed (lookAtOffsetY).
    SetKeyframeLookAt,
    // keyframeRefs + value (lookAtEnabled) + secondaryId (lookAtTarget) + speed (lookAtOffsetY when secondaryTick == 0
    // and speed is finite; set secondaryTick != 0 or NaN speed to preserve each keyframe's existing lookAtOffsetY).
    SetKeyframesLookAt,
    // id + tick: captures the live client camera pose into a Free or Custom Third-Person keyframe.
    CaptureKeyframeFromView,
    AddClip,
    SplitClip,
    MoveAndTrimClip,
    RenameClip,
    SetClipLocked,
    SetClipExportEnabled,
    SetAllClipsExportEnabled,
    ResetClipsToFullRange,
    DeleteClip,
    HealClipCuts,
    AddScene,
    DuplicateScene,
    RenameScene,
    MoveScene,
    DeleteScene,
    SwitchScene,
    SetSceneExportRange,
    AddSpeedTrack,
    RemoveSpeedTrack,
    AddSpeedKeyframe,
    MoveSpeedKeyframe,
    DeleteSpeedKeyframe,
    SetSpeedKeyframeValue,
    SetSpeedKeyframeInterpolation,
    SetSpeedKeyframeBezierControls,
    SetSpeedTrackEnabled,
    SetSpeedTrackLocked,
    ClearSpeedKeyframes,
    AddFreezeTrack,
    RemoveFreezeTrack,
    AddFreezeKeyframe,
    MoveFreezeKeyframe,
    DeleteFreezeKeyframe,
    SetFreezeKeyframe,
    SetFreezeTrackEnabled,
    SetFreezeTrackLocked,
    ClearFreezeKeyframes,
    AddTimelapseTrack,
    RemoveTimelapseTrack,
    // tick + secondaryTick (outputTicks).
    AddTimelapseKeyframe,
    MoveTimelapseKeyframe,
    DeleteTimelapseKeyframe,
    // tick + secondaryTick (outputTicks).
    SetTimelapseKeyframeOutputTicks,
    SetTimelapseTrackEnabled,
    SetTimelapseTrackLocked,
    ClearTimelapseKeyframes,
    AddTimeOfDayTrack,
    RemoveTimeOfDayTrack,
    // tick + secondaryTick (timeOfDay) + kind (CameraInterpolationType).
    AddTimeOfDayKeyframe,
    // tick (fromTick) + secondaryTick (toTick).
    MoveTimeOfDayKeyframe,
    // ticks + secondaryTick (tickDelta).
    MoveTimeOfDayKeyframes,
    // tick.
    RemoveTimeOfDayKeyframe,
    // ticks.
    RemoveTimeOfDayKeyframes,
    // tick + secondaryTick (timeOfDay).
    SetTimeOfDayKeyframeValue,
    // tick + kind (CameraInterpolationType).
    SetTimeOfDayKeyframeInterpolation,
    // ticks + kind (CameraInterpolationType).
    SetTimeOfDayKeyframesInterpolation,
    // tick + bezierCtrl1 + bezierCtrl2.
    SetTimeOfDayKeyframeBezier,
    // value (enabled).
    SetTimeOfDayTrackEnabled,
    // value (locked).
    SetTimeOfDayTrackLocked,
    ClearTimeOfDayKeyframes,
    // value (enabled) + tick (daytimeTicks).
    SetDaytimeOverride,
    // kind (static_cast<int>(WeatherOverride)).
    SetWeatherOverride,
    AddWeatherTrack,
    RemoveWeatherTrack,
    // value (enabled).
    SetWeatherTrackEnabled,
    // value (locked).
    SetWeatherTrackLocked,
    // tick + kind (static_cast<int>(WeatherOverride)).
    AddWeatherKeyframe,
    // tick.
    RemoveWeatherKeyframe,
    // ticks.
    RemoveWeatherKeyframes,
    // tick (fromTick) + secondaryTick (toTick).
    MoveWeatherKeyframe,
    // ticks + secondaryTick (tickDelta).
    MoveWeatherKeyframes,
    // tick + kind (static_cast<int>(WeatherOverride)).
    SetWeatherKeyframeValue,
    // ticks + kind (static_cast<int>(WeatherOverride)).
    SetWeatherKeyframesValue,
    // value (disabled).
    SetDisableServerResourcePacks,
    ReloadReplayAtCurrentTick,
    // name (optional track name).
    AddAudioTrack,
    // id (trackId).
    RemoveAudioTrack,
    // id (trackId) + name.
    RenameAudioTrack,
    // id (trackId) + value (enabled).
    SetAudioTrackEnabled,
    // id (trackId) + value (muted).
    SetAudioTrackMuted,
    // id (trackId) + value (locked).
    SetAudioTrackLocked,
    // id (trackId) + volume.
    SetAudioTrackVolume,
    // id (trackId) + path (filePath) + tick (startTick) + name (optional clip name).
    AddAudioClip,
    // id (trackId) + secondaryId (clipId).
    RemoveAudioClip,
    // id (trackId) + secondaryId (clipId) + tick (startTick).
    MoveAudioClip,
    // id (trackId) + secondaryId (clipId) + path (filePath) + trimStartSeconds + durationSeconds + volume + muted.
    SetAudioClipProps,
};

struct QueuedExportJob {
    exporting::ExportSettings settings;
    std::string               sceneId;
    std::string               label;
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
    float                                    speedValue{1.0f};
    double                                   trimStartSeconds{0.0};
    double                                   durationSeconds{0.0};
    float                                    volume{1.0f};
    int                                      kind{};
    bool                                     value{};
    bool                                     muted{false};
    state::editing::model::Vec3              position{};
    state::editing::model::Vec2              bezierCtrl1{};
    state::editing::model::Vec2              bezierCtrl2{};
    std::optional<exporting::ExportSettings> exportSettings;
    std::vector<QueuedExportJob>             exportJobs;
    std::vector<std::string>                 replayIds;
    // Set only when pasting a copied keyframe. An empty value captures the live camera, as before.
    std::optional<editing::model::CameraKeyframe> keyframe;
    std::optional<editing::model::CameraShake>    shake;
    // Batch keyframe edits (multi-select move / delete / interpolation / paste).
    std::vector<editing::model::KeyframeRef>            keyframeRefs;
    std::vector<state::editing::model::SelectedClipCut> clipCuts;
    std::vector<int>                                    ticks;
    size_t                                   historySteps{1};
};

} // namespace playback::state
