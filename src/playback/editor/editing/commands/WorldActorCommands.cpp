#include "WorldActorCommands.h"

#include "playback/editor/editing/WorldActorOps.h"

#include <utility>

namespace playback::editor::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

} // namespace

SplitWorldActorAtPlayhead::SplitWorldActorAtPlayhead(int tick) : mTick(tick) {}

void SplitWorldActorAtPlayhead::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = !WorldActorOps::splitAt(state.worldActor, mTick).empty();
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        SplitWorldActorAtPlayhead::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SplitWorldActorAtPlayhead::label() const { return "Split World Actor"; }

TrimWorldActorSegment::TrimWorldActorSegment(std::string id, int start, int end)
: mId(std::move(id)),
  mStart(start),
  mEnd(end) {}

void TrimWorldActorSegment::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = WorldActorOps::trimSegment(state.worldActor, mId, mStart, mEnd, state.totalTicks);
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        TrimWorldActorSegment::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string TrimWorldActorSegment::label() const { return "Trim World Actor"; }

SetWorldActorSegmentSpeed::SetWorldActorSegmentSpeed(std::string id, float speed) : mId(std::move(id)), mSpeed(speed) {}

void SetWorldActorSegmentSpeed::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = WorldActorOps::setSpeed(state.worldActor, mId, mSpeed);
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        SetWorldActorSegmentSpeed::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetWorldActorSegmentSpeed::label() const { return "Set World Actor Speed"; }

RippleDeleteWorldActorSeg::RippleDeleteWorldActorSeg(std::string id) : mId(std::move(id)) {}

void RippleDeleteWorldActorSeg::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = WorldActorOps::rippleDelete(state.worldActor, mId, state.totalTicks);
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        RippleDeleteWorldActorSeg::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RippleDeleteWorldActorSeg::label() const { return "Ripple Delete World Actor"; }

} // namespace playback::editor::editing::command
