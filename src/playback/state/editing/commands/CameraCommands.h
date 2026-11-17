#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

// Shifts every referenced keyframe by the same delta. All or nothing: a locked track, a key
// leaving [0, total] or landing on a key that is not moving rejects the whole move.
class MoveKeyframes final : public model::IEditCommand {
public:
    MoveKeyframes(std::vector<model::KeyframeRef> refs, int delta);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::KeyframeRef>      mRefs;
    int                                  mDelta;
    std::optional<model::EditorStateExt> mBefore;
};

// Deletes the referenced keyframes, skipping locked tracks.
class DeleteKeyframes final : public model::IEditCommand {
public:
    explicit DeleteKeyframes(std::vector<model::KeyframeRef> refs);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::KeyframeRef>      mRefs;
    std::optional<model::EditorStateExt> mBefore;
};

class SetKeyframesInterpolation final : public model::IEditCommand {
public:
    SetKeyframesInterpolation(std::vector<model::KeyframeRef> refs, model::CameraInterpolationType interpolation);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::KeyframeRef>      mRefs;
    model::CameraInterpolationType       mInterpolation;
    std::optional<model::EditorStateExt> mBefore;
};

// Inserts keyframes with explicit poses (multi-key paste). Occupied ticks and locked tracks are skipped.
class AddKeyframes final : public model::IEditCommand {
public:
    explicit AddKeyframes(std::vector<model::KeyframeRef> refs);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::KeyframeRef>      mRefs;
    std::optional<model::EditorStateExt> mBefore;
};

class AddFreeCamera final : public model::IEditCommand {
public:
    explicit AddFreeCamera(std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteCamera final : public model::IEditCommand {
public:
    explicit DeleteCamera(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
};

class AddKeyframe final : public model::IEditCommand {
public:
    AddKeyframe(std::string cameraId, int tick);
    AddKeyframe(std::string cameraId, int tick, std::optional<model::CameraKeyframe> captured);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    std::optional<model::CameraKeyframe> mCaptured;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveKeyframe final : public model::IEditCommand {
public:
    MoveKeyframe(std::string cameraId, int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteKeyframe final : public model::IEditCommand {
public:
    DeleteKeyframe(std::string cameraId, int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class SetKeyframeInterpolation final : public model::IEditCommand {
public:
    SetKeyframeInterpolation(std::string cameraId, int tick, model::CameraInterpolationType interpolation);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::CameraInterpolationType       mInterpolation;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraTrackState final : public model::IEditCommand {
public:
    enum class Property : uint8_t { Enabled, Locked };

    SetCameraTrackState(std::string cameraId, Property property, bool value);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    Property                             mProperty;
    bool                                 mValue;
    std::optional<model::EditorStateExt> mBefore;
};

class RenameCamera final : public model::IEditCommand {
public:
    RenameCamera(std::string cameraId, std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class ClearCameraKeyframes final : public model::IEditCommand {
public:
    explicit ClearCameraKeyframes(std::string cameraId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraKeyframePosition final : public model::IEditCommand {
public:
    SetCameraKeyframePosition(std::string cameraId, int tick, model::Vec3 position);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::Vec3                          mPosition;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraKeyframeFov final : public model::IEditCommand {
public:
    SetCameraKeyframeFov(std::string cameraId, int tick, float fov);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    float                                mFov;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraKeyframeSubject final : public model::IEditCommand {
public:
    SetCameraKeyframeSubject(std::string cameraId, int tick, std::string subject);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    std::string                          mSubject;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraKeyframeRotation final : public model::IEditCommand {
public:
    SetCameraKeyframeRotation(std::string cameraId, int tick, model::Vec3 rotation);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::Vec3                          mRotation;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraShake final : public model::IEditCommand {
public:
    SetCameraShake(std::string cameraId, std::optional<model::CameraShake> shake);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::optional<model::CameraShake>    mShake;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraKeyframeBezierControls final : public model::IEditCommand {
public:
    SetCameraKeyframeBezierControls(std::string cameraId, int tick, model::Vec2 ctrl1, model::Vec2 ctrl2);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::Vec2                          mCtrl1;
    model::Vec2                          mCtrl2;
    std::optional<model::EditorStateExt> mBefore;
};

class SetCameraKeyframeMode final : public model::IEditCommand {
public:
    SetCameraKeyframeMode(std::string cameraId, int tick, model::CameraKeyframeMode mode);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTick;
    model::CameraKeyframeMode            mMode;
    std::optional<model::EditorStateExt> mBefore;
};

class SetKeyframesMode final : public model::IEditCommand {
public:
    SetKeyframesMode(std::vector<model::KeyframeRef> refs, model::CameraKeyframeMode mode);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::KeyframeRef>      mRefs;
    model::CameraKeyframeMode            mMode;
    std::optional<model::EditorStateExt> mBefore;
};

class SetKeyframesSubject final : public model::IEditCommand {
public:
    SetKeyframesSubject(std::vector<model::KeyframeRef> refs, std::string subject);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::KeyframeRef>      mRefs;
    std::string                          mSubject;
    std::optional<model::EditorStateExt> mBefore;
};

// Moves a camera to `targetIndex` in the camera list. The list order is the shot priority: a
// camera earlier in the list wins when several have an active keyframe at the same tick.
class MoveCamera final : public model::IEditCommand {
public:
    MoveCamera(std::string cameraId, int targetIndex);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mTargetIndex;
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
