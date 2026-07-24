#include "EditorContext.h"

namespace playback::editor {

EditorState EditorContext::snapshot() const {
    std::scoped_lock lock(mMutex);
    return mState;
}

void EditorContext::publish(EditorState state) {
    std::scoped_lock lock(mMutex);
    mState = state;
}

void EditorContext::submit(EditorAction action) {
    std::scoped_lock lock(mMutex);
    mPendingActions.push_back(action);
}

std::vector<EditorAction> EditorContext::takeActions() {
    std::scoped_lock          lock(mMutex);
    std::vector<EditorAction> actions;
    actions.swap(mPendingActions);
    return actions;
}

void EditorContext::reset() {
    std::scoped_lock lock(mMutex);
    mState = {};
    mPendingActions.clear();
}

} // namespace playback::editor
