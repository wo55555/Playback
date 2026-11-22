#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

class AddTimelapseTrack final : public model::IEditCommand {
public:
    AddTimelapseTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveTimelapseTrack final : public model::IEditCommand {
public:
    RemoveTimelapseTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class AddTimelapseKeyframe final : public model::IEditCommand {
public:
    explicit AddTimelapseKeyframe(int tick, int outputTicks = 0);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    int                                  mOutputTicks;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveTimelapseKeyframe final : public model::IEditCommand {
public:
    MoveTimelapseKeyframe(int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteTimelapseKeyframe final : public model::IEditCommand {
public:
    explicit DeleteTimelapseKeyframe(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimelapseKeyframeOutputTicks final : public model::IEditCommand {
public:
    SetTimelapseKeyframeOutputTicks(int tick, int outputTicks);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    int                                  mOutputTicks;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimelapseTrackState final : public model::IEditCommand {
public:
    enum class Property { Enabled, Locked };

    SetTimelapseTrackState(Property property, bool value);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    Property                             mProperty;
    bool                                 mValue;
    std::optional<model::EditorStateExt> mBefore;
};

class ClearTimelapseKeyframes final : public model::IEditCommand {
public:
    ClearTimelapseKeyframes() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
