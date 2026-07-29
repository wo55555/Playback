#include "Editor.h"

#include "playback/refactor/editor/iconfont.h"

#include "imgui.h"

namespace playback::refactor::editor {

Editor& Editor::getInstance() {
    static Editor instance;
    return instance;
}

void Editor::initialize() {
    KeyMap::initialize();
    mIconSystem.loadFonts();
    mTheme.apply();
    mOpen = false;
}

void Editor::shutdown() {
    mOpen = false;
    mCommandStack.clear();
    mSelection.clear();
    mState = {};
}

void Editor::toggle() {
    mOpen = !mOpen;
    if (mOpen) {
        mTheme.apply();
    }
}

void Editor::draw() {
    if (!mOpen) return;

    // Apply theme each frame
    mTheme.apply();

    // Delegate to current mode
    if (mModeManager.current() == EditorMode::Edit) {
        mEditMode.draw();
    } else {
        mRenderMode.draw();
    }

    // Draw error dialog overlay (if active)
    ErrorDialog::getInstance().draw();
}

} // namespace playback::refactor::editor