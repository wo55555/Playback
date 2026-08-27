#include "WorldActorCommands.h"

#include "playback/state/editing/WorldActorOps.h"

#include <utility>

namespace playback::state::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

} // namespace

SplitWorldActorAtPlayhead::SplitWorldActorAtPlayhead(int tick) : mTick(tick) {}

void SplitWorldActorAtPlayhead::execute(model::EditorStateExt& state) {
    auto before = state;
    if (WorldActorOps::splitAt(state.worldActor, mTick).empty()) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        SplitWorldActorAtPlayhead::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SplitWorldActorAtPlayhead::label() const { return "Split World Actor"; }

TrimWorldActorSegment::TrimWorldActorSegment(std::string id, int start, int end)
: mId(std::move(id)),
  mStart(start),
  mEnd(end) {}

void TrimWorldActorSegment::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!WorldActorOps::trimSegment(state.worldActor, mId, mStart, mEnd, state.totalTicks)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        TrimWorldActorSegment::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string TrimWorldActorSegment::label() const { return "Trim World Actor"; }

SetWorldActorSegmentSpeed::SetWorldActorSegmentSpeed(std::string id, float speed) : mId(std::move(id)), mSpeed(speed) {}

void SetWorldActorSegmentSpeed::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!WorldActorOps::setSpeed(state.worldActor, mId, mSpeed)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        SetWorldActorSegmentSpeed::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetWorldActorSegmentSpeed::label() const { return "Set World Actor Speed"; }

RippleDeleteWorldActorSeg::RippleDeleteWorldActorSeg(std::string id) : mId(std::move(id)) {}

void RippleDeleteWorldActorSeg::execute(model::EditorStateExt& state) {
    auto before = state;
    if (!WorldActorOps::rippleDelete(state.worldActor, mId, state.totalTicks)) {
        mBefore.reset();
        return;
    }
    mBefore = std::move(before);
}

void        RippleDeleteWorldActorSeg::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RippleDeleteWorldActorSeg::label() const { return "Ripple Delete World Actor"; }

} // namespace playback::state::editing::command
