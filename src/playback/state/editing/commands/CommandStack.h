#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class CommandStack {
public:
    bool push(std::unique_ptr<model::IEditCommand> cmd, model::EditorStateExt& state);
    bool undo(model::EditorStateExt& state, std::string* undoneLabel = nullptr);
    bool redo(model::EditorStateExt& state, std::string* redoneLabel = nullptr);
    void clear();

    void               markSaved();
    [[nodiscard]] bool isAtSavedState() const;

    [[nodiscard]] size_t maxSteps() const { return mMaxSteps; }
    void                 setMaxSteps(size_t maxSteps);

    [[nodiscard]] std::vector<std::string> undoLabels() const;
    [[nodiscard]] std::vector<std::string> redoLabels() const;
    [[nodiscard]] std::vector<std::string> recentUndoLabels(size_t maxCount) const;
    [[nodiscard]] std::vector<std::string> recentRedoLabels(size_t maxCount) const;
    [[nodiscard]] size_t                   undoCount() const;
    [[nodiscard]] size_t                   redoCount() const;
    [[nodiscard]] std::string              undoLabel() const;
    [[nodiscard]] std::string              redoLabel() const;
    [[nodiscard]] bool                     canUndo() const;
    [[nodiscard]] bool                     canRedo() const;

    // Bumped by every mutation that took effect, including undo and redo.
    [[nodiscard]] std::uint64_t revision() const { return mRevision; }

private:
    struct Entry {
        std::unique_ptr<model::IEditCommand> cmd;
        std::string                          targetSceneId;
        std::uint64_t                        beforeStateId{};
        std::uint64_t                        afterStateId{};
    };

    std::vector<Entry> mUndo;
    std::vector<Entry> mRedo;
    size_t             mMaxSteps{256};
    std::uint64_t      mRevision{};
    std::uint64_t      mNextStateId{1};
    std::uint64_t      mCurrentStateId{0};
    std::uint64_t      mSavedStateId{0};
};

} // namespace playback::state::editing::command
