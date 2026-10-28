#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

// Adds a marker at `tick` unless one already sits there. An empty id picks the next free "marker_N".
class AddMarker final : public model::IEditCommand {
public:
    AddMarker(std::string id, int tick, std::string label);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    int                                  mTick;
    std::string                          mLabel;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteMarker final : public model::IEditCommand {
public:
    explicit DeleteMarker(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
};

// Moves a marker; rejected when another marker already occupies the target tick.
class MoveMarker final : public model::IEditCommand {
public:
    MoveMarker(std::string id, int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class RenameMarker final : public model::IEditCommand {
public:
    RenameMarker(std::string id, std::string label);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::string                          mLabel;
    std::optional<model::EditorStateExt> mBefore;
};

// Next unused "marker_N" id for the project.
[[nodiscard]] std::string nextMarkerId(model::EditorStateExt const& state);

} // namespace playback::state::editing::command
