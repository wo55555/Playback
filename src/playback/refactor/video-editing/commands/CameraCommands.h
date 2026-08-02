#pragma once
#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/IEditCommand.h"
#include <optional>
#include <string>
namespace playback::refactor::video_editing {
class AddFreeCamera final : public editor::IEditCommand { public: explicit AddFreeCamera(std::string); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mName; std::optional<editor::EditorStateExt> mBefore; };
class DeleteCamera final : public editor::IEditCommand { public: explicit DeleteCamera(std::string); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<editor::EditorStateExt> mBefore; };
class CreateBindingCamera final : public editor::IEditCommand { public: CreateBindingCamera(std::string,std::string); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mSubActorId,mName; std::optional<editor::EditorStateExt> mBefore; };
class UnbindCamera final : public editor::IEditCommand { public: explicit UnbindCamera(std::string); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<editor::EditorStateExt> mBefore; };
class AddKeyframe final : public editor::IEditCommand { public: AddKeyframe(std::string,int); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId; int mTick; std::optional<editor::EditorStateExt> mBefore; };
class MoveKeyframe final : public editor::IEditCommand { public: MoveKeyframe(std::string,std::string,int); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; int mTick; std::optional<editor::EditorStateExt> mBefore; };
class DeleteKeyframe final : public editor::IEditCommand { public: DeleteKeyframe(std::string,std::string); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; std::optional<editor::EditorStateExt> mBefore; };
class SetKeyframeEasing final : public editor::IEditCommand { public: SetKeyframeEasing(std::string,std::string,editor::EasingType); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId,mKeyframeId; editor::EasingType mEasing; std::optional<editor::EditorStateExt> mBefore; };
class SetCameraKind final : public editor::IEditCommand { public: SetCameraKind(std::string,editor::CameraKind); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mCameraId; editor::CameraKind mKind; std::optional<editor::EditorStateExt> mBefore; };
}
