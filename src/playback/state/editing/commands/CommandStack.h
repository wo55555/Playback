#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class CommandStack {
public:
    void push(std::unique_ptr<model::IEditCommand> cmd, model::EditorStateExt& state);
    bool undo(model::EditorStateExt& state);
    bool redo(model::EditorStateExt& state);
    void clear();

    [[nodiscard]] std::vector<std::string> undoLabels() const;
    [[nodiscard]] std::vector<std::string> redoLabels() const;
    [[nodiscard]] bool                     canUndo() const;
    [[nodiscard]] bool                     canRedo() const;

    // Bumped by every mutation that took effect, including undo and redo.
    [[nodiscard]] std::uint64_t revision() const { return mRevision; }

private:
    std::vector<std::unique_ptr<model::IEditCommand>> mUndo;
    std::vector<std::unique_ptr<model::IEditCommand>> mRedo;
    size_t                                            mMaxSteps{100};
    std::uint64_t                                     mRevision{};
};

} // namespace playback::state::editing::command
