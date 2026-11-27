#include "CommandStack.h"

#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <algorithm>
#include <utility>

namespace playback::state::editing::command {

namespace {

struct RuntimeSnapshot {
    int   totalTicks{};
    int   currentTick{};
    bool  playing{};
    float playbackSpeed{1.0f};
};

RuntimeSnapshot captureRuntime(model::EditorStateExt const& state) noexcept {
    return {state.totalTicks, state.currentTick, state.playing, state.playbackSpeed};
}

void restoreRuntime(model::EditorStateExt& state, RuntimeSnapshot const& saved) {
    state.currentTick   = saved.currentTick;
    state.playing       = saved.playing;
    state.playbackSpeed = saved.playbackSpeed;
    if (saved.totalTicks > state.totalTicks) {
        int const previousTotalTicks = state.totalTicks;
        state.totalTicks             = saved.totalTicks;
        ClipOps::syncTotalTicks(state, previousTotalTicks);
    }
}

} // namespace

bool CommandStack::push(std::unique_ptr<model::IEditCommand> cmd, model::EditorStateExt& state) {
    if (!cmd) return false;

    ClipOps::syncActiveSceneFromWorking(state);
    std::string const targetSceneId = state.activeSceneId;

    cmd->execute(state);
    if (!cmd->didChange()) return false;

    ClipOps::syncActiveSceneFromWorking(state);
    std::uint64_t const afterStateId = mNextStateId++;
    mUndo.push_back(Entry{std::move(cmd), targetSceneId, mCurrentStateId, afterStateId});
    mCurrentStateId = afterStateId;
    mRedo.clear();
    ++mRevision;

    if (mUndo.size() > mMaxSteps) {
        mUndo.erase(mUndo.begin(), mUndo.begin() + static_cast<std::ptrdiff_t>(mUndo.size() - mMaxSteps));
    }
    return true;
}

bool CommandStack::undo(model::EditorStateExt& state, std::string* undoneLabel) {
    if (mUndo.empty()) return false;

    auto const saved = captureRuntime(state);
    ClipOps::syncActiveSceneFromWorking(state);

    auto entry = std::move(mUndo.back());
    mUndo.pop_back();
    if (undoneLabel) *undoneLabel = entry.cmd->label();

    entry.cmd->undo(state);
    ClipOps::syncActiveSceneFromWorking(state);
    restoreRuntime(state, saved);

    mCurrentStateId = entry.beforeStateId;
    mRedo.push_back(std::move(entry));
    ++mRevision;
    return true;
}

bool CommandStack::redo(model::EditorStateExt& state, std::string* redoneLabel) {
    if (mRedo.empty()) return false;

    auto const saved = captureRuntime(state);
    ClipOps::syncActiveSceneFromWorking(state);

    auto entry = std::move(mRedo.back());
    mRedo.pop_back();

    if (!entry.targetSceneId.empty() && state.activeSceneId != entry.targetSceneId) {
        if (ClipOps::findScene(state, entry.targetSceneId) != nullptr) {
            state.activeSceneId = entry.targetSceneId;
            ClipOps::loadWorkingFromActiveScene(state);
        }
    }

    entry.cmd->execute(state);
    if (!entry.cmd->didChange()) {
        mRedo.clear();
        restoreRuntime(state, saved);
        return false;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    restoreRuntime(state, saved);

    if (redoneLabel) *redoneLabel = entry.cmd->label();
    mCurrentStateId = entry.afterStateId;
    mUndo.push_back(std::move(entry));
    ++mRevision;
    return true;
}

void CommandStack::clear() {
    mUndo.clear();
    mRedo.clear();
    mCurrentStateId = mNextStateId++;
    mSavedStateId   = mCurrentStateId;
    ++mRevision;
}

void CommandStack::markSaved() { mSavedStateId = mCurrentStateId; }

bool CommandStack::isAtSavedState() const { return mCurrentStateId == mSavedStateId; }

void CommandStack::setMaxSteps(size_t maxSteps) {
    mMaxSteps = std::max<size_t>(1, maxSteps);
    if (mUndo.size() > mMaxSteps) {
        mUndo.erase(mUndo.begin(), mUndo.begin() + static_cast<std::ptrdiff_t>(mUndo.size() - mMaxSteps));
    }
}

std::vector<std::string> CommandStack::undoLabels() const {
    std::vector<std::string> labels;
    labels.reserve(mUndo.size());
    for (const auto& entry : mUndo) {
        labels.push_back(entry.cmd->label());
    }
    return labels;
}

std::vector<std::string> CommandStack::redoLabels() const {
    std::vector<std::string> labels;
    labels.reserve(mRedo.size());
    for (const auto& entry : mRedo) {
        labels.push_back(entry.cmd->label());
    }
    return labels;
}

std::vector<std::string> CommandStack::recentUndoLabels(size_t maxCount) const {
    std::vector<std::string> labels;
    size_t const             count = std::min(maxCount, mUndo.size());
    labels.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        labels.push_back(mUndo[mUndo.size() - 1 - i].cmd->label());
    }
    return labels;
}

std::vector<std::string> CommandStack::recentRedoLabels(size_t maxCount) const {
    std::vector<std::string> labels;
    size_t const             count = std::min(maxCount, mRedo.size());
    labels.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        labels.push_back(mRedo[mRedo.size() - 1 - i].cmd->label());
    }
    return labels;
}

size_t CommandStack::undoCount() const { return mUndo.size(); }
size_t CommandStack::redoCount() const { return mRedo.size(); }

std::string CommandStack::undoLabel() const { return mUndo.empty() ? std::string{} : mUndo.back().cmd->label(); }

std::string CommandStack::redoLabel() const { return mRedo.empty() ? std::string{} : mRedo.back().cmd->label(); }

bool CommandStack::canUndo() const { return !mUndo.empty(); }
bool CommandStack::canRedo() const { return !mRedo.empty(); }

} // namespace playback::state::editing::command
