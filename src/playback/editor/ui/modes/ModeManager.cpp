#include "playback/editor/ui/modes/ModeManager.h"

namespace playback::editor::ui {

ModeManager& ModeManager::getInstance() {
    static ModeManager instance;
    return instance;
}

void ModeManager::switchTo(EditorMode mode) {
    if (mCurrent == mode) return;
    mCurrent         = mode;
    mTransitioning   = true;
    mTransitionAlpha = 0.0f; // Will be animated over 200ms
    // onModeChanged.emit(mode);
}

} // namespace playback::editor::ui