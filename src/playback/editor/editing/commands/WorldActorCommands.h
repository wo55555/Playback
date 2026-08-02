#pragma once
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"
#include <optional>
#include <string>
namespace playback::editor::editing::command {
class SplitWorldActorAtPlayhead final : public model::IEditCommand { public: explicit SplitWorldActorAtPlayhead(int); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: int mTick; std::optional<model::EditorStateExt> mBefore; };
class TrimWorldActorSegment final : public model::IEditCommand { public: TrimWorldActorSegment(std::string,int,int); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; int mStart,mEnd; std::optional<model::EditorStateExt> mBefore; };
class SetWorldActorSegmentSpeed final : public model::IEditCommand { public: SetWorldActorSegmentSpeed(std::string,float); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; float mSpeed; std::optional<model::EditorStateExt> mBefore; };
class RippleDeleteWorldActorSeg final : public model::IEditCommand { public: explicit RippleDeleteWorldActorSeg(std::string); void execute(model::EditorStateExt&) override; void undo(model::EditorStateExt&) override; std::string label() const override; private: std::string mId; std::optional<model::EditorStateExt> mBefore; };
}
