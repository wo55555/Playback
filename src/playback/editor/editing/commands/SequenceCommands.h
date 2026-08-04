#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::editor::editing::command {
class AddCameraSequence final : public model::IEditCommand { public: void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::optional<model::EditorStateExt> mBefore; };
class DeleteCameraSequence final : public model::IEditCommand { public: void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::optional<model::EditorStateExt> mBefore; };
class SplitSequenceAtPlayhead final : public model::IEditCommand { public: explicit SplitSequenceAtPlayhead(int tick); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: int mTick; std::optional<model::EditorStateExt> mBefore; };
class TrimSequenceSegment final : public model::IEditCommand { public: TrimSequenceSegment(std::string id, int start, int end); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; int mStart, mEnd; std::optional<model::EditorStateExt> mBefore; };
class DeleteSequenceSegment final : public model::IEditCommand { public: explicit DeleteSequenceSegment(std::string id); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<model::EditorStateExt> mBefore; };
class BindSequenceToCamera final : public model::IEditCommand { public: BindSequenceToCamera(std::string id, std::string cameraId); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId, mCameraId; std::optional<model::EditorStateExt> mBefore; };
}
