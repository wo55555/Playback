#pragma once

#include "playback/editor/graphics/ReplayUILayout.h"
#include "playback/editor/ui/PanelContext.h"
#include "playback/state/EditorAction.h"
#include "playback/state/EditorState.h"
#include "playback/state/editing/models/SelectionModel.h"

#include "EditorTheme.h"
#include "modes/EditMode.h"
#include "modes/RenderMode.h"
#include "panels/DetailsPanel.h"
#include "panels/StatusPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ViewportPanel.h"
#include "playback/editor/ui/components/EditorToast.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/EditorMenuBar.h"
#include "playback/editor/ui/modes/ModeManager.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace playback::editor::ui {

class ReplayEditor : public EditorCommands {
public:
    static ReplayEditor& getInstance();

    void initialize();
    void shutdown();
    void saveLayoutPreferences() const;

    void draw(playback::state::EditorState const& state, SubmitAction const& submit);

    void handleKeyboardShortcuts();
    void updateFollowExitTap();

    [[nodiscard]] playback::state::EditorState const&          state() const;
    [[nodiscard]] state::editing::model::SelectionModel const& selection() const { return mSelection; }
    state::editing::model::SelectionModel&                     selection() { return mSelection; }
    void                                                       submitAction(playback::state::EditorAction action) const;

    void               seekTo(int tick) override;
    void               seekRelative(int tickDelta) override;
    void                       undo(std::size_t steps = 1) override;
    void                       redo(std::size_t steps = 1) override;
    void               toggleViewportMaximized() override { mViewportMaximized = !mViewportMaximized; }
    [[nodiscard]] bool isViewportMaximized() const override { return mViewportMaximized; }
    void               toggleCameraPath() override;
    [[nodiscard]] bool isCameraPathVisible() const override { return mCameraPathVisible; }
    bool               deleteSelection() override;
    bool               addKeyframeAtPlayhead() override;
    bool               selectAllKeyframes() override;
    bool               copySelection() override;
    bool               pasteAtPlayhead() override;
    bool               addMarkerAtPlayhead() override;
    bool                       canSplitClipAtPlayhead() override;
    bool                       splitClipAtPlayhead() override;
    bool                       canTrimClipAtPlayhead(bool trimIn) override;
    bool                       trimClipAtPlayhead(bool trimIn) override;
    bool                       toggleSelectedClipExport() override;
    [[nodiscard]] TimelineTool timelineTool() const override { return mTimelinePanel.tool(); }
    void                       setTimelineTool(TimelineTool tool) override { mTimelinePanel.setTool(tool); }
    bool                       selectCameraByIndex(std::size_t index);

    [[nodiscard]] bool  isInfoOverlayVisible() const override { return mViewportPanel.isInfoOverlayVisible(); }
    void                      setInfoOverlayVisible(bool visible) override;
    [[nodiscard]] bool  isAutoPreviewEnabled() const override { return mViewportPanel.isAutoPreviewEnabled(); }
    void                      setAutoPreviewEnabled(bool enabled) override;
    [[nodiscard]] bool  isSnapEnabled() const override { return mTimelinePanel.isSnapEnabled(); }
    void                      setSnapEnabled(bool enabled) override;
    [[nodiscard]] float videoAspectRatio() const override { return mVideoAspectRatio; }
    void                setVideoAspectRatio(float ratio) override;
    void                openExportDialog() override;
    void                      openExportSingleClip(std::string const& clipId) override;
    void                      openExportScene(std::string const& sceneId) override;
    [[nodiscard]] int         exportStartTick() const override;
    [[nodiscard]] int         exportEndTick() const override;
    [[nodiscard]] int         exportFps() const override { return mMenuBar.exportFps(); }
    void                      setExportRange(int inTick, int outTick) override;
    void                      markExportPoint(bool isIn) override;
    void                      clearExportRange() override;
    [[nodiscard]] UiScaleTier uiScaleTier() const override { return mUiScaleTier; }
    void                      setUiScaleTier(UiScaleTier tier) override;

    [[nodiscard]] bool  isHudElementShown(visuals::HudElement element) const override;
    void                setHudElementShown(visuals::HudElement element, bool shown) override;
    [[nodiscard]] bool  isPreviewAudioMuted() const override;
    void                setPreviewAudioMuted(bool muted) override;
    [[nodiscard]] float previewAudioVolume() const override;
    void                setPreviewAudioVolume(float volume, bool commit) override;
    [[nodiscard]] bool  isPreviewSilentWhilePaused() const override;
    void                setPreviewSilentWhilePaused(bool silent) override;

    void                      setGameTexture(ImTextureID texture) { mViewportPanel.setGameTexture(texture); }
    [[nodiscard]] ImTextureID gameTexture() const { return mViewportPanel.gameTexture(); }
    [[nodiscard]] Rect        viewportVideoRect() const { return mViewportPanel.videoRect(); }
    [[nodiscard]] Rect        viewportOverlayRect() const { return mViewportPanel.overlayRect(); }

private:
    ReplayEditor() = default;

    [[nodiscard]] PanelContext frameContext();

    bool mViewportMaximized{false};
    bool mCameraPathVisible{true};

    ModeManager&  mModeManager{ModeManager::getInstance()};
    EditorMenuBar mMenuBar;
    Splitter      mSplitter;

    ViewportPanel mViewportPanel;
    DetailsPanel  mDetailsPanel;
    TimelinePanel mTimelinePanel;
    StatusPanel   mStatusPanel;

    EditMode   mEditMode;
    RenderMode mRenderMode;

    playback::state::EditorState const*   mFrameState{};
    SubmitAction const*                   mSubmit{};
    state::editing::model::SelectionModel mSelection;
    exporting::ExportState                mLastExportState{exporting::ExportState::Idle};
    EditorToast                           mToast;
    std::uint64_t                         mLastHistoryEventSeq{0};

    bool  mFollowExitTapArmed{false};
    bool  mExitHoldBlocked{false};
    float mExitHoldSeconds{};

    float       mDetailsWidthRatio{0.22f};
    float       mTimelineHeightRatio{0.24f};
    float       mVideoAspectRatio{16.0f / 9.0f};
    UiScaleTier mUiScaleTier{UiScaleTier::Auto};

    struct TimelineViewPreferences {
        float zoomScale{1.0f};
        float horizontalScroll{};
    };

    std::map<std::string, TimelineViewPreferences> mTimelineViewPreferences;
    std::string                                    mActiveReplayPath;

    void loadLayoutPreferences();
    void syncTimelineViewPreferences(std::string_view replayPath);
    void updateExitHold(bool escapeConsumedByUi);

    friend class EditMode;
    friend class RenderMode;
};

} // namespace playback::editor::ui
