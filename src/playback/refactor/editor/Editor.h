#pragma once

#include "models/EditorStateExt.h"
#include "models/SelectionModel.h"
#include "CommandStack.h"
#include "EditorTheme.h"
#include "HintBar.h"
#include "IconSystem.h"
#include "KeyMap.h"
#include "MenuBar.h"
#include "ModeManager.h"
#include "Splitter.h"
#include "panels/DetailsPanel.h"
#include "panels/StatusPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ViewportPanel.h"
#include "render/EditMode.h"
#include "render/RenderMode.h"

namespace playback::refactor::editor {

class Editor {
public:
    static Editor& getInstance();

    // Core lifecycle
    void initialize();
    void shutdown();
    void toggle();
    [[nodiscard]] bool isOpen() const { return mOpen; }

    // Main draw entry (called from ImGui render loop)
    void draw();

    // Accessors
    EditorStateExt&       state() { return mState; }
    const EditorStateExt& state() const { return mState; }
    SelectionModel&       selection() { return mSelection; }
    CommandStack&         commandStack() { return mCommandStack; }

private:
    Editor() = default;

    bool mOpen{false};

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

    // Modes
    EditMode        mEditMode;
    RenderMode      mRenderMode;

    // Data
    EditorStateExt  mState;
    SelectionModel  mSelection;
    CommandStack    mCommandStack;

    // Layout
    float mTimelineRatio{0.40f}; // 20% ~ 70%
};

} // namespace playback::refactor::editor