#pragma once
#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/IEditCommand.h"
#include <optional>
#include <string>
namespace playback::refactor::video_editing {
class SplitWorldActorAtPlayhead final : public editor::IEditCommand { public: explicit SplitWorldActorAtPlayhead(int); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: int mTick; std::optional<editor::EditorStateExt> mBefore; };
class TrimWorldActorSegment final : public editor::IEditCommand { public: TrimWorldActorSegment(std::string,int,int); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; int mStart,mEnd; std::optional<editor::EditorStateExt> mBefore; };
class SetWorldActorSegmentSpeed final : public editor::IEditCommand { public: SetWorldActorSegmentSpeed(std::string,float); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; float mSpeed; std::optional<editor::EditorStateExt> mBefore; };
class RippleDeleteWorldActorSeg final : public editor::IEditCommand { public: explicit RippleDeleteWorldActorSeg(std::string); void execute(editor::EditorStateExt&) override; void undo(editor::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<editor::EditorStateExt> mBefore; };
}
