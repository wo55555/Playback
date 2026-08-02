#pragma once

#include <string>

namespace playback::editor::editing::model {

// Forward declarations
struct EditorStateExt;

// Base interface for undo/redo commands
class IEditCommand {
public:
    virtual ~IEditCommand() = default;

    virtual void execute(EditorStateExt& state) = 0;
    virtual void undo(EditorStateExt& state) = 0;
    [[nodiscard]] virtual std::string label() const = 0;
};

} // namespace playback::editor::editing::model