#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

class AddFreeCamera final : public model::IEditCommand {
public:
    explicit AddFreeCamera(std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class DeleteCamera final : public model::IEditCommand {
public:
    explicit DeleteCamera(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class CreateBindingCamera final : public model::IEditCommand {
public:
    CreateBindingCamera(std::string subActorId, std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mSubActorId;
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class UnbindCamera final : public model::IEditCommand {
public:
    explicit UnbindCamera(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class AddKeyframe final : public model::IEditCommand {
public:
    AddKeyframe(std::string cameraId, int tick);
    AddKeyframe(std::string cameraId, int tick, std::optional<model::CameraKeyframe> captured);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    std::optional<model::CameraKeyframe> mCaptured;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class MoveKeyframe final : public model::IEditCommand {
public:
    MoveKeyframe(std::string cameraId, int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class DeleteKeyframe final : public model::IEditCommand {
public:
    DeleteKeyframe(std::string cameraId, int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class SetKeyframeInterpolation final : public model::IEditCommand {
public:
    SetKeyframeInterpolation(std::string cameraId, int tick, model::CameraInterpolationType interpolation);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::CameraInterpolationType       mInterpolation;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class SetCameraTrackState final : public model::IEditCommand {
public:
    enum class Property : uint8_t { Enabled };

    SetCameraTrackState(std::string cameraId, Property property, bool value);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    Property                             mProperty;
    bool                                 mValue;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class SetCameraKeyframePosition final : public model::IEditCommand {
public:
    SetCameraKeyframePosition(std::string cameraId, int tick, model::Vec3 position);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::Vec3                          mPosition;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class SetCameraKeyframeFov final : public model::IEditCommand {
public:
    SetCameraKeyframeFov(std::string cameraId, int tick, float fov);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    float                                mFov;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

} // namespace playback::state::editing::command
