#include "SpeedCommands.h"

#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playback::state::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

int clampTick(int tick, int totalTicks) noexcept { return std::clamp(tick, 0, std::max(0, totalTicks)); }

float clampUnitOrDefault(float value, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : fallback;
}

} // namespace

void AddSpeedTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.isVisible()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                  = state;
    state.speedTrack.present = true;
    state.speedTrack.enabled = true;
    state.speedTrack.locked  = false;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddSpeedTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddSpeedTrack::label() const { return "playback.refactorEditor.history.addSpeedTrack"; }

void RemoveSpeedTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!state.speedTrack.isVisible() || state.speedTrack.locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                  = state;
    state.speedTrack.present = false;
    state.speedTrack.enabled = true;
    state.speedTrack.locked  = false;
    state.speedTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveSpeedTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveSpeedTrack::label() const { return "playback.refactorEditor.history.removeSpeedTrack"; }

AddSpeedKeyframe::AddSpeedKeyframe(int tick, float speed) : mTick(tick), mSpeed(speed) {}

void AddSpeedKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    int const tick = clampTick(mTick, state.totalTicks);
    if (state.speedTrack.locked || state.speedTrack.keysByTick.contains(tick)) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    model::SpeedKeyframe key;
    key.speed                = model::clampSpeedKeyframeValue(mSpeed);
    state.speedTrack.present = true;
    state.speedTrack.keysByTick.emplace(tick, key);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddSpeedKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddSpeedKeyframe::label() const { return "playback.refactorEditor.history.addSpeedKeyframe"; }

MoveSpeedKeyframe::MoveSpeedKeyframe(int fromTick, int toTick) : mFromTick(fromTick), mToTick(toTick) {}

void MoveSpeedKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.locked) return;

    int const  fromTick = clampTick(mFromTick, state.totalTicks);
    int const  toTick   = clampTick(mToTick, state.totalTicks);
    auto const source   = state.speedTrack.keysByTick.find(fromTick);
    if (source == state.speedTrack.keysByTick.end() || fromTick == toTick
        || state.speedTrack.keysByTick.contains(toTick)) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore  = state;
    auto key = std::move(source->second);
    state.speedTrack.keysByTick.erase(source);
    state.speedTrack.keysByTick.emplace(toTick, std::move(key));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveSpeedKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveSpeedKeyframe::label() const { return "playback.refactorEditor.history.moveSpeedKeyframe"; }

DeleteSpeedKeyframe::DeleteSpeedKeyframe(int tick) : mTick(tick) {}

void DeleteSpeedKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.locked) return;

    auto const it = state.speedTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.speedTrack.keysByTick.end()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.speedTrack.keysByTick.erase(it);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        DeleteSpeedKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteSpeedKeyframe::label() const { return "playback.refactorEditor.history.deleteSpeedKeyframe"; }

SetSpeedKeyframeValue::SetSpeedKeyframeValue(int tick, float speed) : mTick(tick), mSpeed(speed) {}

void SetSpeedKeyframeValue::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.locked) return;

    auto const it = state.speedTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.speedTrack.keysByTick.end()) return;

    float const clamped = model::clampSpeedKeyframeValue(mSpeed);
    if (std::abs(it->second.speed - clamped) < 1e-6f) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore          = state;
    it->second.speed = clamped;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSpeedKeyframeValue::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSpeedKeyframeValue::label() const { return "playback.refactorEditor.history.setSpeedKeyframeValue"; }

SetSpeedKeyframeInterpolation::SetSpeedKeyframeInterpolation(int tick, model::CameraInterpolationType interpolation)
: mTick(tick),
  mInterpolation(interpolation) {}

void SetSpeedKeyframeInterpolation::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.locked) return;
    if (static_cast<int>(mInterpolation) < 0
        || static_cast<int>(mInterpolation) > static_cast<int>(model::CameraInterpolationType::CubicBezier)) {
        return;
    }

    auto const it = state.speedTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.speedTrack.keysByTick.end() || it->second.interpolationType == mInterpolation) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    it->second.interpolationType = mInterpolation;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSpeedKeyframeInterpolation::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSpeedKeyframeInterpolation::label() const {
    return "playback.refactorEditor.history.setSpeedKeyframeInterpolation";
}

SetSpeedKeyframeBezierControls::SetSpeedKeyframeBezierControls(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2)
: mTick(tick),
  mCtrl1(ctrl1),
  mCtrl2(ctrl2) {}

void SetSpeedKeyframeBezierControls::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.locked) return;

    auto const it = state.speedTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.speedTrack.keysByTick.end()) return;

    model::Vec2 const ctrl1{clampUnitOrDefault(mCtrl1.x, 0.42f), clampUnitOrDefault(mCtrl1.y, 0.0f)};
    model::Vec2 const ctrl2{clampUnitOrDefault(mCtrl2.x, 0.58f), clampUnitOrDefault(mCtrl2.y, 1.0f)};
    if (it->second.bezierCtrl1.x == ctrl1.x && it->second.bezierCtrl1.y == ctrl1.y
        && it->second.bezierCtrl2.x == ctrl2.x && it->second.bezierCtrl2.y == ctrl2.y) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                = state;
    it->second.bezierCtrl1 = ctrl1;
    it->second.bezierCtrl2 = ctrl2;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSpeedKeyframeBezierControls::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSpeedKeyframeBezierControls::label() const {
    return "playback.refactorEditor.history.setSpeedKeyframeBezierControls";
}

SetSpeedTrackState::SetSpeedTrackState(Property property, bool value) : mProperty(property), mValue(value) {}

void SetSpeedTrackState::execute(model::EditorStateExt& state) {
    mBefore.reset();
    bool* target = mProperty == Property::Locked ? &state.speedTrack.locked : &state.speedTrack.enabled;
    if (*target == mValue) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    *target = mValue;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSpeedTrackState::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSpeedTrackState::label() const {
    return mProperty == Property::Locked ? "playback.refactorEditor.history.setSpeedTrackLocked"
                                         : "playback.refactorEditor.history.setSpeedTrackEnabled";
}

void ClearSpeedKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.speedTrack.locked || state.speedTrack.keysByTick.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.speedTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        ClearSpeedKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string ClearSpeedKeyframes::label() const { return "playback.refactorEditor.history.clearSpeedKeyframes"; }

} // namespace playback::state::editing::command
