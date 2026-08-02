#pragma once

#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::refactor::video_editing {
class SplitSequenceAtPlayhead final : public editor::IEditCommand { public: explicit SplitSequenceAtPlayhead(int tick); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: int mTick; std::optional<editor::EditorStateExt> mBefore; };
class TrimSequenceSegment final : public editor::IEditCommand { public: TrimSequenceSegment(std::string id, int start, int end); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; int mStart, mEnd; std::optional<editor::EditorStateExt> mBefore; };
class DeleteSequenceSegment final : public editor::IEditCommand { public: explicit DeleteSequenceSegment(std::string id); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<editor::EditorStateExt> mBefore; };
class BindSequenceToCamera final : public editor::IEditCommand { public: BindSequenceToCamera(std::string id, std::string cameraId); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId, mCameraId; std::optional<editor::EditorStateExt> mBefore; };
}
