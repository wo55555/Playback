#pragma once

#include "models/IEditCommand.h"

#include <memory>
#include <string>
#include <vector>

namespace playback::refactor::editor {

class CommandStack {
public:
    void push(std::unique_ptr<IEditCommand> cmd);
    bool undo();
    bool redo();
    void clear();

    [[nodiscard]] std::vector<std::string> undoLabels() const;
    [[nodiscard]] std::vector<std::string> redoLabels() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;

private:
    std::vector<std::unique_ptr<IEditCommand>> mUndo;
    std::vector<std::unique_ptr<IEditCommand>> mRedo;
    size_t mMaxSteps{100};
};

} // namespace playback::refactor::editor