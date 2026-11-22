#include "TimelapseCommands.h"

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

void AddTimelapseTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timelapseTrack.isVisible()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    state.timelapseTrack.present = true;
    state.timelapseTrack.enabled = true;
    state.timelapseTrack.locked  = false;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddTimelapseTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddTimelapseTrack::label() const { return "playback.refactorEditor.history.addTimelapseTrack"; }

void RemoveTimelapseTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!state.timelapseTrack.isVisible() || state.timelapseTrack.locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    state.timelapseTrack.present = false;
    state.timelapseTrack.enabled = true;
    state.timelapseTrack.locked  = false;
    state.timelapseTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveTimelapseTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveTimelapseTrack::label() const { return "playback.refactorEditor.history.removeTimelapseTrack"; }

AddTimelapseKeyframe::AddTimelapseKeyframe(int tick, int outputTicks) : mTick(tick), mOutputTicks(outputTicks) {}

void AddTimelapseKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    int const tick = clampTick(mTick, state.totalTicks);
    if (state.timelapseTrack.locked || state.timelapseTrack.keysByTick.contains(tick)) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    model::TimelapseKeyframe key;
    key.outputTicks              = model::clampTimelapseOutputTicks(mOutputTicks);
    state.timelapseTrack.present = true;
    state.timelapseTrack.keysByTick.emplace(tick, key);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddTimelapseKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddTimelapseKeyframe::label() const { return "playback.refactorEditor.history.addTimelapseKeyframe"; }

MoveTimelapseKeyframe::MoveTimelapseKeyframe(int fromTick, int toTick) : mFromTick(fromTick), mToTick(toTick) {}

void MoveTimelapseKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timelapseTrack.locked) return;

    int const  fromTick = clampTick(mFromTick, state.totalTicks);
    int const  toTick   = clampTick(mToTick, state.totalTicks);
    auto const source   = state.timelapseTrack.keysByTick.find(fromTick);
    if (source == state.timelapseTrack.keysByTick.end() || fromTick == toTick
        || state.timelapseTrack.keysByTick.contains(toTick)) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore  = state;
    auto key = std::move(source->second);
    state.timelapseTrack.keysByTick.erase(source);
    state.timelapseTrack.keysByTick.emplace(toTick, std::move(key));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveTimelapseKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveTimelapseKeyframe::label() const { return "playback.refactorEditor.history.moveTimelapseKeyframe"; }

DeleteTimelapseKeyframe::DeleteTimelapseKeyframe(int tick) : mTick(tick) {}

void DeleteTimelapseKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timelapseTrack.locked) return;

    auto const it = state.timelapseTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.timelapseTrack.keysByTick.end()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.timelapseTrack.keysByTick.erase(it);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        DeleteTimelapseKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteTimelapseKeyframe::label() const { return "playback.refactorEditor.history.deleteTimelapseKeyframe"; }

SetTimelapseKeyframeOutputTicks::SetTimelapseKeyframeOutputTicks(int tick, int outputTicks)
: mTick(tick),
  mOutputTicks(outputTicks) {}

void SetTimelapseKeyframeOutputTicks::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timelapseTrack.locked) return;

    auto const it = state.timelapseTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.timelapseTrack.keysByTick.end()) return;

    int const clampedOutputTicks = model::clampTimelapseOutputTicks(mOutputTicks);
    if (it->second.outputTicks == clampedOutputTicks) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                = state;
    it->second.outputTicks = clampedOutputTicks;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimelapseKeyframeOutputTicks::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimelapseKeyframeOutputTicks::label() const {
    return "playback.refactorEditor.history.setTimelapseKeyframe";
}

SetTimelapseTrackState::SetTimelapseTrackState(Property property, bool value) : mProperty(property), mValue(value) {}

void SetTimelapseTrackState::execute(model::EditorStateExt& state) {
    mBefore.reset();
    bool* target = mProperty == Property::Locked ? &state.timelapseTrack.locked : &state.timelapseTrack.enabled;
    if (*target == mValue) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    *target = mValue;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimelapseTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimelapseTrackState::label() const {
    return mProperty == Property::Locked ? "playback.refactorEditor.history.setTimelapseTrackLocked"
                                         : "playback.refactorEditor.history.setTimelapseTrackEnabled";
}

void ClearTimelapseKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timelapseTrack.locked || state.timelapseTrack.keysByTick.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.timelapseTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        ClearTimelapseKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string ClearTimelapseKeyframes::label() const { return "playback.refactorEditor.history.clearTimelapseKeyframes"; }

} // namespace playback::state::editing::command
