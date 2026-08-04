#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::editor::editing::command {
class AddFreeCamera final : public model::IEditCommand { public: explicit AddFreeCamera(std::string); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mName; std::optional<model::EditorStateExt> mBefore; };
class DeleteCamera final : public model::IEditCommand { public: explicit DeleteCamera(std::string); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<model::EditorStateExt> mBefore; };
class CreateBindingCamera final : public model::IEditCommand { public: CreateBindingCamera(std::string,std::string); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mSubActorId,mName; std::optional<model::EditorStateExt> mBefore; };
class UnbindCamera final : public model::IEditCommand { public: explicit UnbindCamera(std::string); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<model::EditorStateExt> mBefore; };
class AddKeyframe final : public model::IEditCommand { public: AddKeyframe(std::string,int); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId; int mTick; std::optional<model::EditorStateExt> mBefore; };
class CaptureCameraKeyframe final : public model::IEditCommand { public: CaptureCameraKeyframe(std::string,int,model::Vec3,float,float,float); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId; int mTick; model::Vec3 mPosition; float mYaw,mPitch,mFov; std::optional<model::EditorStateExt> mBefore; };
class MoveKeyframe final : public model::IEditCommand { public: MoveKeyframe(std::string,std::string,int); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; int mTick; std::optional<model::EditorStateExt> mBefore; };
class DeleteKeyframe final : public model::IEditCommand { public: DeleteKeyframe(std::string,std::string); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; std::optional<model::EditorStateExt> mBefore; };
class SetKeyframeEasing final : public model::IEditCommand { public: SetKeyframeEasing(std::string,std::string,model::EasingType); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; model::EasingType mEasing; std::optional<model::EditorStateExt> mBefore; };
class ApplyCameraTransitionPreset final : public model::IEditCommand { public: ApplyCameraTransitionPreset(std::string,std::string,model::CameraTransitionPreset); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; model::CameraTransitionPreset mPreset; std::optional<model::EditorStateExt> mBefore; };
class SetCameraKind final : public model::IEditCommand { public: SetCameraKind(std::string,model::CameraKind); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId; model::CameraKind mKind; std::optional<model::EditorStateExt> mBefore; };
}
