#pragma once

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
#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/EditorMenuBar.h"
#include "playback/editor/ui/modes/ModeManager.h"

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

    void draw(playback::state::EditorState const& state, SubmitAction const& submit);

    void handleKeyboardShortcuts();

    [[nodiscard]] playback::state::EditorState const&          state() const;
    [[nodiscard]] state::editing::model::SelectionModel const& selection() const { return mSelection; }
    state::editing::model::SelectionModel&                     selection() { return mSelection; }
    void                                                       submitAction(playback::state::EditorAction action) const;
    void                                                       openExportDialog();

    void               seekTo(int tick) override;
    void               seekRelative(int tickDelta) override;
    void               toggleViewportMaximized() override { mViewportMaximized = !mViewportMaximized; }
    [[nodiscard]] bool isViewportMaximized() const override { return mViewportMaximized; }
    bool               deleteSelection() override;
    bool               addKeyframeAtPlayhead() override;

    void                      setGameTexture(ImTextureID texture) { mViewportPanel.setGameTexture(texture); }
    [[nodiscard]] ImTextureID gameTexture() const { return mViewportPanel.gameTexture(); }
    void                      setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] float       videoAspectRatio() const { return mVideoAspectRatio; }
    [[nodiscard]] Rect        viewportVideoRect() const { return mViewportPanel.videoRect(); }

private:
    ReplayEditor() = default;

    [[nodiscard]] PanelContext frameContext();

    bool mViewportMaximized{false};

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

    float mExitHoldSeconds{};

    float mDetailsWidthRatio{0.28f};
    float mTimelineHeightRatio{0.35f};
    float mVideoAspectRatio{16.0f / 9.0f};

    struct TimelineViewPreferences {
        float zoomScale{1.0f};
        float horizontalScroll{};
    };

    std::map<std::string, TimelineViewPreferences> mTimelineViewPreferences;
    std::string                                    mActiveReplayPath;

    void loadLayoutPreferences();
    void saveLayoutPreferences() const;
    void syncTimelineViewPreferences(std::string_view replayPath);
    void updateExitHold();

    friend class EditMode;
    friend class RenderMode;
};

} // namespace playback::editor::ui
