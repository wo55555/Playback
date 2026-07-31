#pragma once

#include "models/EditorStateExt.h"
#include "models/SelectionModel.h"
#include "EditorBridge.h"
#include "EditorTheme.h"
#include "EventBus.h"
#include "HintBar.h"
#include "IconSystem.h"
#include "KeyMap.h"
#include "MenuBar.h"
#include "ModeManager.h"
#include "Splitter.h"
#include "panels/CurveEditorPanel.h"
#include "panels/DetailsPanel.h"
#include "panels/StatusPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ViewportPanel.h"
#include "render/EditMode.h"
#include "render/RenderMode.h"

#include <string>

namespace playback::refactor::editor {

class Editor {
public:
    static Editor& getInstance();

    // Core lifecycle
    void initialize();
    void shutdown();
    void toggle();
    void open();         // Ensure editor is open (no-op if already open)
    [[nodiscard]] bool isOpen() const { return mOpen; }

    // Main draw entry (called from ImGui render loop)
    void draw();

    // Keyboard shortcut processing
    void handleKeyboardShortcuts();

    // Accessors
    EditorStateExt&       state() { return mState; }
    const EditorStateExt& state() const { return mState; }
    SelectionModel&       selection() { return mSelection; }
    CurveEditorPanel&     curveEditorPanel() { return mCurveEditorPanel; }
    void setGameTexture(ImTextureID texture) {
        mViewportPanel.setGameTexture(texture);
    }
    void setVideoAspectRatio(float aspectRatio);
    [[nodiscard]] float videoAspectRatio() const { return mVideoAspectRatio; }
    [[nodiscard]] Rect viewportVideoRect() const { return mViewportPanel.videoRect(); }
    void toggleViewportMaximized() { mViewportMaximized = !mViewportMaximized; }
    [[nodiscard]] bool isViewportMaximized() const { return mViewportMaximized; }

private:
    Editor() = default;

    bool mOpen{false};
    bool mViewportMaximized{false};

    // Core components
    EditorTheme     mTheme;
    IconSystem&     mIconSystem{IconSystem::getInstance()};
    ModeManager&    mModeManager{ModeManager::getInstance()};
    MenuBar         mMenuBar;
    HintBar         mHintBar;
    Splitter        mSplitter;

    // Panels
    ViewportPanel   mViewportPanel;
    DetailsPanel    mDetailsPanel;
    TimelinePanel   mTimelinePanel;
    StatusPanel     mStatusPanel;
    CurveEditorPanel mCurveEditorPanel;

    // Modes
    EditMode        mEditMode;
    RenderMode      mRenderMode;

    // Data
    EditorStateExt  mState;
    SelectionModel  mSelection;

    // Layout
    float mDetailsWidthRatio{0.28f};
    float mTimelineHeightRatio{0.35f};
    float mVideoAspectRatio{16.0f / 9.0f};

    void loadLayoutPreferences();
    void saveLayoutPreferences() const;

    // Allow EditMode to access Editor members
    friend class EditMode;
    friend class RenderMode;
};

} // namespace playback::refactor::editor
