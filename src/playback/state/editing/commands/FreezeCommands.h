#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

class AddFreezeTrack final : public model::IEditCommand {
public:
    AddFreezeTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveFreezeTrack final : public model::IEditCommand {
public:
    RemoveFreezeTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class AddFreezeKeyframe final : public model::IEditCommand {
public:
    explicit AddFreezeKeyframe(int tick, bool frozen = true, int frozenDelay = 0);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    bool                                 mFrozen;
    int                                  mFrozenDelay;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveFreezeKeyframe final : public model::IEditCommand {
public:
    MoveFreezeKeyframe(int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteFreezeKeyframe final : public model::IEditCommand {
public:
    explicit DeleteFreezeKeyframe(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class SetFreezeKeyframe final : public model::IEditCommand {
public:
    SetFreezeKeyframe(int tick, bool frozen, int frozenDelay);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    bool                                 mFrozen;
    int                                  mFrozenDelay;
    std::optional<model::EditorStateExt> mBefore;
};

class SetFreezeTrackState final : public model::IEditCommand {
public:
    enum class Property { Enabled, Locked };

    SetFreezeTrackState(Property property, bool value);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    Property                             mProperty;
    bool                                 mValue;
    std::optional<model::EditorStateExt> mBefore;
};

class ClearFreezeKeyframes final : public model::IEditCommand {
public:
    ClearFreezeKeyframes() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
