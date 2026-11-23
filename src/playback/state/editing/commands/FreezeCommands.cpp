#include "FreezeCommands.h"

#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <utility>

namespace playback::state::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

int clampTick(int tick, int totalTicks) noexcept { return std::clamp(tick, 0, std::max(0, totalTicks)); }

} // namespace

void AddFreezeTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.freezeTrack.isVisible()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                   = state;
    state.freezeTrack.present = true;
    state.freezeTrack.enabled = true;
    state.freezeTrack.locked  = false;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddFreezeTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddFreezeTrack::label() const { return "playback.refactorEditor.history.addFreezeTrack"; }

void RemoveFreezeTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!state.freezeTrack.isVisible() || state.freezeTrack.locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                   = state;
    state.freezeTrack.present = false;
    state.freezeTrack.enabled = true;
    state.freezeTrack.locked  = false;
    state.freezeTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveFreezeTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveFreezeTrack::label() const { return "playback.refactorEditor.history.removeFreezeTrack"; }

AddFreezeKeyframe::AddFreezeKeyframe(int tick, bool frozen, int frozenDelay)
: mTick(tick),
  mFrozen(frozen),
  mFrozenDelay(frozenDelay) {}

void AddFreezeKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    int const tick = clampTick(mTick, state.totalTicks);
    if (state.freezeTrack.locked || state.freezeTrack.keysByTick.contains(tick)) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    model::FreezeKeyframe key;
    key.frozen                = mFrozen;
    key.frozenDelay           = model::clampFreezeDelayTicks(mFrozenDelay);
    state.freezeTrack.present = true;
    state.freezeTrack.keysByTick.emplace(tick, key);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddFreezeKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddFreezeKeyframe::label() const { return "playback.refactorEditor.history.addFreezeKeyframe"; }

MoveFreezeKeyframe::MoveFreezeKeyframe(int fromTick, int toTick) : mFromTick(fromTick), mToTick(toTick) {}

void MoveFreezeKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.freezeTrack.locked) return;

    int const  fromTick = clampTick(mFromTick, state.totalTicks);
    int const  toTick   = clampTick(mToTick, state.totalTicks);
    auto const source   = state.freezeTrack.keysByTick.find(fromTick);
    if (source == state.freezeTrack.keysByTick.end() || fromTick == toTick
        || state.freezeTrack.keysByTick.contains(toTick)) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore  = state;
    auto key = std::move(source->second);
    state.freezeTrack.keysByTick.erase(source);
    state.freezeTrack.keysByTick.emplace(toTick, std::move(key));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveFreezeKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveFreezeKeyframe::label() const { return "playback.refactorEditor.history.moveFreezeKeyframe"; }

DeleteFreezeKeyframe::DeleteFreezeKeyframe(int tick) : mTick(tick) {}

void DeleteFreezeKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.freezeTrack.locked) return;

    auto const it = state.freezeTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.freezeTrack.keysByTick.end()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.freezeTrack.keysByTick.erase(it);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        DeleteFreezeKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteFreezeKeyframe::label() const { return "playback.refactorEditor.history.deleteFreezeKeyframe"; }

SetFreezeKeyframe::SetFreezeKeyframe(int tick, bool frozen, int frozenDelay)
: mTick(tick),
  mFrozen(frozen),
  mFrozenDelay(frozenDelay) {}

void SetFreezeKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.freezeTrack.locked) return;

    auto const it = state.freezeTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.freezeTrack.keysByTick.end()) return;

    int const clampedDelay = model::clampFreezeDelayTicks(mFrozenDelay);
    if (it->second.frozen == mFrozen && it->second.frozenDelay == clampedDelay) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                = state;
    it->second.frozen      = mFrozen;
    it->second.frozenDelay = clampedDelay;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetFreezeKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetFreezeKeyframe::label() const { return "playback.refactorEditor.history.setFreezeKeyframe"; }

SetFreezeTrackState::SetFreezeTrackState(Property property, bool value) : mProperty(property), mValue(value) {}

void SetFreezeTrackState::execute(model::EditorStateExt& state) {
    mBefore.reset();
    bool* target = mProperty == Property::Locked ? &state.freezeTrack.locked : &state.freezeTrack.enabled;
    if (*target == mValue) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    *target = mValue;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetFreezeTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetFreezeTrackState::label() const {
    return mProperty == Property::Locked ? "playback.refactorEditor.history.setFreezeTrackLocked"
                                         : "playback.refactorEditor.history.setFreezeTrackEnabled";
}

void ClearFreezeKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.freezeTrack.locked || state.freezeTrack.keysByTick.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.freezeTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        ClearFreezeKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string ClearFreezeKeyframes::label() const { return "playback.refactorEditor.history.clearFreezeKeyframes"; }

} // namespace playback::state::editing::command
