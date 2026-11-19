#pragma once

#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/editor/graphics/ReplayUILayout.h"
#include "playback/state/EditorAction.h"
#include "playback/state/EditorState.h"
#include "playback/state/editing/models/SelectionModel.h"
#include "playback/visuals/ReplayHudVisibility.h"

#include <functional>
#include <string>
#include <utility>

namespace playback::editor::ui {

enum class TimelineTool { Select, Razor, Pen };

using SubmitAction = std::function<void(playback::state::EditorAction)>;

// Editor-level behaviour a panel may invoke, kept behind an interface so panels need no singleton.
class EditorCommands {
public:
    virtual ~EditorCommands() = default;

    virtual void               seekTo(int tick)            = 0;
    virtual void               seekRelative(int tickDelta) = 0;
    virtual void               toggleViewportMaximized()   = 0;
    [[nodiscard]] virtual bool isViewportMaximized() const = 0;
    virtual void               toggleCameraPath()          = 0;
    [[nodiscard]] virtual bool isCameraPathVisible() const = 0;
    virtual bool               deleteSelection()           = 0;
    virtual bool               addKeyframeAtPlayhead()     = 0;
    virtual bool               selectAllKeyframes()        = 0;
    virtual bool               copySelection()             = 0;
    virtual bool               pasteAtPlayhead()           = 0;
    virtual bool               addMarkerAtPlayhead()       = 0;
    virtual bool                       canSplitClipAtPlayhead()           = 0;
    virtual bool                       splitClipAtPlayhead()              = 0;
    virtual bool                       canTrimClipAtPlayhead(bool trimIn) = 0;
    virtual bool                       trimClipAtPlayhead(bool trimIn)    = 0;
    virtual bool                       toggleSelectedClipExport()         = 0;
    [[nodiscard]] virtual TimelineTool timelineTool() const               = 0;
    virtual void                       setTimelineTool(TimelineTool tool) = 0;

    // View preferences the inspector's settings page edits; they live in the panels that consume them.
    [[nodiscard]] virtual bool  isInfoOverlayVisible() const        = 0;
    virtual void                setInfoOverlayVisible(bool visible) = 0;
    [[nodiscard]] virtual bool  isAutoPreviewEnabled() const        = 0;
    virtual void                setAutoPreviewEnabled(bool enabled) = 0;
    [[nodiscard]] virtual bool  isSnapEnabled() const               = 0;
    virtual void                setSnapEnabled(bool enabled)        = 0;
    [[nodiscard]] virtual float videoAspectRatio() const            = 0;
    virtual void                setVideoAspectRatio(float ratio)    = 0;
    virtual void                openExportDialog()                  = 0;
    virtual void                openExportSingleClip(std::string const& clipId) = 0;
    virtual void                openExportScene(std::string const& sceneId)     = 0;
    // Shows the selected camera-switch segment in the inspector with its subject search focused.

    // The export range belongs to the active scene and is drawn on the timeline as in/out markers.
    [[nodiscard]] virtual int exportStartTick() const                 = 0;
    [[nodiscard]] virtual int exportEndTick() const                   = 0;
    [[nodiscard]] virtual int exportFps() const                       = 0;
    virtual void              setExportRange(int inTick, int outTick) = 0;
    virtual void              markExportPoint(bool isIn)              = 0;
    virtual void              clearExportRange()                      = 0;

    [[nodiscard]] virtual UiScaleTier uiScaleTier() const              = 0;
    virtual void                      setUiScaleTier(UiScaleTier tier) = 0;

    // What the replay shows and plays; the HUD choices carry over to an export, the audio ones are preview-only.
    [[nodiscard]] virtual bool  isHudElementShown(visuals::HudElement element) const        = 0;
    virtual void                setHudElementShown(visuals::HudElement element, bool shown) = 0;
    [[nodiscard]] virtual bool  isPreviewAudioMuted() const                                 = 0;
    virtual void                setPreviewAudioMuted(bool muted)                            = 0;
    [[nodiscard]] virtual float previewAudioVolume() const                                  = 0;
    // `commit` is false while a slider is still being dragged: the change is heard at once but saved on release.
    virtual void               setPreviewAudioVolume(float volume, bool commit) = 0;
    [[nodiscard]] virtual bool isPreviewSilentWhilePaused() const               = 0;
    virtual void               setPreviewSilentWhilePaused(bool silent)         = 0;
};

// Everything a panel needs for one frame, so panels stay callable without the editor singleton.
struct PanelContext {
    playback::state::EditorState const&             state;
    state::editing::model::SelectionModel&          selection;
    SubmitAction const&                             submit;
    EditorCommands&                                 commands;
    std::optional<graphics::RenderCameraProjection> cameraProjection;

    void submitAction(playback::state::EditorAction action) const {
        if (submit) submit(std::move(action));
    }

    [[nodiscard]] state::editing::model::EditorStateExt const* project() const { return state.project.get(); }
};

} // namespace playback::editor::ui
