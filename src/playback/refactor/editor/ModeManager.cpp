#include "ModeManager.h"

namespace playback::refactor::editor {

ModeManager& ModeManager::getInstance() {
    static ModeManager instance;
    return instance;
}

void ModeManager::switchTo(EditorMode mode) {
    if (mCurrent == mode) return;
    mCurrent          = mode;
    mTransitioning    = true;
    mTransitionAlpha  = 0.0f; // Will be animated over 200ms
    // onModeChanged.emit(mode);
}

} // namespace playback::refactor::editor