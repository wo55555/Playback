#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::editor::editing::command {

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
    MoveKeyframe(std::string cameraId, std::string keyframeId, int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::string                          mKeyframeId;
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class DeleteKeyframe final : public model::IEditCommand {
public:
    DeleteKeyframe(std::string cameraId, std::string keyframeId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::string                          mKeyframeId;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class SetKeyframeInterpolation final : public model::IEditCommand {
public:
    SetKeyframeInterpolation(
        std::string                    cameraId,
        std::string                    keyframeId,
        model::CameraInterpolationType interpolation
    );
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::string                          mKeyframeId;
    model::CameraInterpolationType       mInterpolation;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class SetCameraTrackState final : public model::IEditCommand {
public:
    enum class Property : uint8_t { Enabled, PathVisible };

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

class SetCameraKind final : public model::IEditCommand {
public:
    SetCameraKind(std::string cameraId, model::CameraKind kind);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    model::CameraKind                    mKind;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

} // namespace playback::editor::editing::command
