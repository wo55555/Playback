#include "EditorContext.h"

#include <algorithm>
#include <utility>

namespace playback::state {

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

void EditorContext::undo(size_t steps) {
    EditorAction action;
    action.type         = EditorActionType::UndoEditorEdit;
    action.historySteps = std::max<size_t>(1, steps);
    submit(std::move(action));
}

void EditorContext::redo(size_t steps) {
    EditorAction action;
    action.type         = EditorActionType::RedoEditorEdit;
    action.historySteps = std::max<size_t>(1, steps);
    submit(std::move(action));
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

} // namespace playback::state
