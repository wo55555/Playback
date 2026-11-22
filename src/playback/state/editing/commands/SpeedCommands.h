#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

class AddSpeedTrack final : public model::IEditCommand {
public:
    AddSpeedTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveSpeedTrack final : public model::IEditCommand {
public:
    RemoveSpeedTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class AddSpeedKeyframe final : public model::IEditCommand {
public:
    explicit AddSpeedKeyframe(int tick, float speed = 1.0f);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    float                                mSpeed;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveSpeedKeyframe final : public model::IEditCommand {
public:
    MoveSpeedKeyframe(int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteSpeedKeyframe final : public model::IEditCommand {
public:
    explicit DeleteSpeedKeyframe(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class SetSpeedKeyframeValue final : public model::IEditCommand {
public:
    SetSpeedKeyframeValue(int tick, float speed);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    float                                mSpeed;
    std::optional<model::EditorStateExt> mBefore;
};

class SetSpeedKeyframeInterpolation final : public model::IEditCommand {
public:
    SetSpeedKeyframeInterpolation(int tick, model::CameraInterpolationType interpolation);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    model::CameraInterpolationType       mInterpolation;
    std::optional<model::EditorStateExt> mBefore;
};

class SetSpeedKeyframeBezierControls final : public model::IEditCommand {
public:
    SetSpeedKeyframeBezierControls(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    model::Vec2                          mCtrl1;
    model::Vec2                          mCtrl2;
    std::optional<model::EditorStateExt> mBefore;
};

class SetSpeedTrackState final : public model::IEditCommand {
public:
    enum class Property { Enabled, Locked };

    SetSpeedTrackState(Property property, bool value);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    Property                             mProperty;
    bool                                 mValue;
    std::optional<model::EditorStateExt> mBefore;
};

class ClearSpeedKeyframes final : public model::IEditCommand {
public:
    ClearSpeedKeyframes() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
