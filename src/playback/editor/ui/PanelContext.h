#pragma once

#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/editor/graphics/ReplayUILayout.h"
#include "playback/state/EditorAction.h"
#include "playback/state/EditorState.h"
#include "playback/state/editing/models/SelectionModel.h"

#include <functional>
#include <utility>

namespace playback::editor::ui {

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

    [[nodiscard]] virtual UiScaleTier uiScaleTier() const              = 0;
    virtual void                      setUiScaleTier(UiScaleTier tier) = 0;
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
