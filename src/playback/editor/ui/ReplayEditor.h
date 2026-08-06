#pragma once

#include "playback/editor/context/EditorAction.h"
#include "playback/editor/context/EditorState.h"
#include "playback/editor/editing/models/SelectionModel.h"

#include "EditorTheme.h"
#include "HintBar.h"
#include "modes/EditMode.h"
#include "modes/RenderMode.h"
#include "panels/CurveEditorPanel.h"
#include "panels/DetailsPanel.h"
#include "panels/StatusPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ViewportPanel.h"
#include "playback/editor/input/KeyMap.h"
#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/menus/EditorMenuBar.h"
#include "playback/editor/ui/modes/ModeManager.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace playback::editor::ui {

class ReplayEditor {
public:
    using SubmitAction = std::function<void(playback::editor::EditorAction)>;

    static ReplayEditor& getInstance();

    // Core lifecycle
    void initialize();
    void shutdown();

    void draw(playback::editor::EditorState const& state, SubmitAction const& submit);

    // Keyboard shortcut processing
    void handleKeyboardShortcuts();

    [[nodiscard]] playback::editor::EditorState const&  state() const;
    [[nodiscard]] editing::model::SelectionModel const& selection() const { return mSelection; }
    editing::model::SelectionModel&                     selection() { return mSelection; }
    void                                                submitAction(playback::editor::EditorAction action) const;
    CurveEditorPanel&                                   curveEditorPanel() { return mCurveEditorPanel; }
    void                setGameTexture(ImTextureID texture) { mViewportPanel.setGameTexture(texture); }
    void                setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] float videoAspectRatio() const { return mVideoAspectRatio; }
    [[nodiscard]] Rect  viewportVideoRect() const { return mViewportPanel.videoRect(); }
    void                toggleViewportMaximized() { mViewportMaximized = !mViewportMaximized; }
    [[nodiscard]] bool  isViewportMaximized() const { return mViewportMaximized; }

private:
    ReplayEditor() = default;

    bool mViewportMaximized{false};

    // Core components
    EditorTheme   mTheme;
    ModeManager&  mModeManager{ModeManager::getInstance()};
    EditorMenuBar mMenuBar;
    HintBar       mHintBar;
    Splitter      mSplitter;

    // Panels
    ViewportPanel    mViewportPanel;
    DetailsPanel     mDetailsPanel;
    TimelinePanel    mTimelinePanel;
    StatusPanel      mStatusPanel;
    CurveEditorPanel mCurveEditorPanel;

    // Modes
    EditMode   mEditMode;
    RenderMode mRenderMode;

    playback::editor::EditorState const* mFrameState{};
    SubmitAction const*                  mSubmit{};
    editing::model::SelectionModel       mSelection;

    // Layout
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

    // Allow EditMode to access ReplayEditor members
    friend class EditMode;
    friend class RenderMode;
};

} // namespace playback::editor::ui
