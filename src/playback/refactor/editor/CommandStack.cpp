#include "CommandStack.h"

#include "models/EditorStateExt.h"

namespace playback::refactor::editor {

void CommandStack::push(std::unique_ptr<IEditCommand> cmd, EditorStateExt& state) {
    // Execute the command first
    cmd->execute(state);
    mUndo.push_back(std::move(cmd));
    mRedo.clear();

    // Trim to max steps
    if (mUndo.size() > mMaxSteps) {
        mUndo.erase(mUndo.begin(), mUndo.begin() + (mUndo.size() - mMaxSteps));
    }
}

bool CommandStack::undo(EditorStateExt& state) {
    if (mUndo.empty()) return false;
    auto cmd = std::move(mUndo.back());
    mUndo.pop_back();
    cmd->undo(state);
    mRedo.push_back(std::move(cmd));
    return true;
}

bool CommandStack::redo(EditorStateExt& state) {
    if (mRedo.empty()) return false;
    auto cmd = std::move(mRedo.back());
    mRedo.pop_back();
    cmd->execute(state);
    mUndo.push_back(std::move(cmd));
    return true;
}

void CommandStack::clear() {
    mUndo.clear();
    mRedo.clear();
}

std::vector<std::string> CommandStack::undoLabels() const {
    std::vector<std::string> labels;
    labels.reserve(mUndo.size());
    for (const auto& cmd : mUndo) {
        labels.push_back(cmd->label());
    }
    return labels;
}

std::vector<std::string> CommandStack::redoLabels() const {
    std::vector<std::string> labels;
    labels.reserve(mRedo.size());
    for (const auto& cmd : mRedo) {
        labels.push_back(cmd->label());
    }
    return labels;
}

bool CommandStack::canUndo() const { return !mUndo.empty(); }
bool CommandStack::canRedo() const { return !mRedo.empty(); }

} // namespace playback::refactor::editor