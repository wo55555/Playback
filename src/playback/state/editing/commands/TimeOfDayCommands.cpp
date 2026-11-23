#include "TimeOfDayCommands.h"

#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <cmath>
#include <set>
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

bool isValidInterpolationType(model::CameraInterpolationType type) noexcept {
    return static_cast<int>(type) >= 0
        && static_cast<int>(type) <= static_cast<int>(model::CameraInterpolationType::CubicBezier);
}

bool isValidWeatherOverride(model::WeatherOverride weather) noexcept {
    return static_cast<int>(weather) >= 0
        && static_cast<int>(weather) <= static_cast<int>(model::WeatherOverride::Thunder);
}

} // namespace

void AddTimeOfDayTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.isVisible()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    state.timeOfDayTrack.present = true;
    state.timeOfDayTrack.enabled = true;
    state.timeOfDayTrack.locked  = false;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddTimeOfDayTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddTimeOfDayTrack::label() const { return "playback.refactorEditor.history.addTimeOfDayTrack"; }

void RemoveTimeOfDayTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!state.timeOfDayTrack.isVisible() || state.timeOfDayTrack.locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    state.timeOfDayTrack.present = false;
    state.timeOfDayTrack.enabled = true;
    state.timeOfDayTrack.locked  = false;
    state.timeOfDayTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveTimeOfDayTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveTimeOfDayTrack::label() const { return "playback.refactorEditor.history.removeTimeOfDayTrack"; }

SetTimeOfDayTrackEnabled::SetTimeOfDayTrackEnabled(bool enabled) : mEnabled(enabled) {}

void SetTimeOfDayTrackEnabled::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.enabled == mEnabled) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    state.timeOfDayTrack.enabled = mEnabled;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimeOfDayTrackEnabled::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimeOfDayTrackEnabled::label() const {
    return "playback.refactorEditor.history.setTimeOfDayTrackEnabled";
}

SetTimeOfDayTrackLocked::SetTimeOfDayTrackLocked(bool locked) : mLocked(locked) {}

void SetTimeOfDayTrackLocked::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked == mLocked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                     = state;
    state.timeOfDayTrack.locked = mLocked;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimeOfDayTrackLocked::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimeOfDayTrackLocked::label() const { return "playback.refactorEditor.history.setTimeOfDayTrackLocked"; }

AddTimeOfDayKeyframe::AddTimeOfDayKeyframe(int tick, int timeOfDay, model::CameraInterpolationType interpolationType)
: mTick(tick),
  mTimeOfDay(timeOfDay),
  mInterpolationType(interpolationType) {}

void AddTimeOfDayKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    mReplaced = false;
    if (state.timeOfDayTrack.locked) return;

    int const tick = clampTick(mTick, state.totalTicks);
    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    auto const               existing = state.timeOfDayTrack.keysByTick.find(tick);
    model::TimeOfDayKeyframe key;
    if (existing != state.timeOfDayTrack.keysByTick.end()) {
        mReplaced = true;
        key       = existing->second;
        if (mInterpolationType != model::CameraInterpolationType::Linear
            && isValidInterpolationType(mInterpolationType)) {
            key.interpolationType = mInterpolationType;
        }
    } else if (isValidInterpolationType(mInterpolationType)) {
        key.interpolationType = mInterpolationType;
    }
    key.timeOfDay = model::clampTimeOfDayTicks(mTimeOfDay);

    state.timeOfDayTrack.present = true;
    state.timeOfDayTrack.keysByTick.insert_or_assign(tick, key);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddTimeOfDayKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddTimeOfDayKeyframe::label() const {
    return mReplaced ? "playback.refactorEditor.history.replaceTimeOfDayKeyframe"
                     : "playback.refactorEditor.history.addTimeOfDayKeyframe";
}

RemoveTimeOfDayKeyframe::RemoveTimeOfDayKeyframe(int tick) : mTick(tick) {}

void RemoveTimeOfDayKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked) return;

    auto const it = state.timeOfDayTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.timeOfDayTrack.keysByTick.end()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.timeOfDayTrack.keysByTick.erase(it);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveTimeOfDayKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveTimeOfDayKeyframe::label() const { return "playback.refactorEditor.history.removeTimeOfDayKeyframe"; }

RemoveTimeOfDayKeyframes::RemoveTimeOfDayKeyframes(std::vector<int> ticks) : mTicks(std::move(ticks)) {}

void RemoveTimeOfDayKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked || mTicks.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    auto before  = state;
    bool changed = false;
    for (int const tick : mTicks) {
        changed |= state.timeOfDayTrack.keysByTick.erase(tick) > 0;
    }
    if (!changed) return;

    mBefore = std::move(before);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveTimeOfDayKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveTimeOfDayKeyframes::label() const {
    return "playback.refactorEditor.history.removeTimeOfDayKeyframes";
}

MoveTimeOfDayKeyframe::MoveTimeOfDayKeyframe(int fromTick, int toTick) : mFromTick(fromTick), mToTick(toTick) {}

void MoveTimeOfDayKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked) return;

    int const  fromTick = clampTick(mFromTick, state.totalTicks);
    int const  toTick   = clampTick(mToTick, state.totalTicks);
    auto const source   = state.timeOfDayTrack.keysByTick.find(fromTick);
    if (source == state.timeOfDayTrack.keysByTick.end() || fromTick == toTick
        || state.timeOfDayTrack.keysByTick.contains(toTick)) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore  = state;
    auto key = std::move(source->second);
    state.timeOfDayTrack.keysByTick.erase(source);
    state.timeOfDayTrack.keysByTick.emplace(toTick, std::move(key));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveTimeOfDayKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveTimeOfDayKeyframe::label() const { return "playback.refactorEditor.history.moveTimeOfDayKeyframe"; }

MoveTimeOfDayKeyframes::MoveTimeOfDayKeyframes(std::vector<int> ticks, int tickDelta)
: mTicks(std::move(ticks)),
  mTickDelta(tickDelta) {}

void MoveTimeOfDayKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked || mTickDelta == 0 || mTicks.empty()) return;

    std::set<int> moving(mTicks.begin(), mTicks.end());
    for (int const tick : moving) {
        int const target = tick + mTickDelta;
        if (!state.timeOfDayTrack.keysByTick.contains(tick) || target < 0 || target > state.totalTicks) return;
        if (state.timeOfDayTrack.keysByTick.contains(target) && !moving.contains(target)) return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    std::vector<std::pair<int, model::TimeOfDayKeyframe>> lifted;
    lifted.reserve(moving.size());
    for (int const tick : moving) {
        auto node = state.timeOfDayTrack.keysByTick.extract(tick);
        lifted.emplace_back(tick + mTickDelta, std::move(node.mapped()));
    }
    for (auto& [tick, key] : lifted) {
        state.timeOfDayTrack.keysByTick.emplace(tick, std::move(key));
    }
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveTimeOfDayKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveTimeOfDayKeyframes::label() const { return "playback.refactorEditor.history.moveTimeOfDayKeyframes"; }

SetTimeOfDayKeyframeValue::SetTimeOfDayKeyframeValue(int tick, int timeOfDay) : mTick(tick), mTimeOfDay(timeOfDay) {}

void SetTimeOfDayKeyframeValue::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked) return;

    auto const it = state.timeOfDayTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.timeOfDayTrack.keysByTick.end()) return;

    int const clamped = model::clampTimeOfDayTicks(mTimeOfDay);
    if (it->second.timeOfDay == clamped) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore              = state;
    it->second.timeOfDay = clamped;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimeOfDayKeyframeValue::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimeOfDayKeyframeValue::label() const {
    return "playback.refactorEditor.history.setTimeOfDayKeyframeValue";
}

SetTimeOfDayKeyframeInterpolation::SetTimeOfDayKeyframeInterpolation(int tick, model::CameraInterpolationType type)
: mTick(tick),
  mType(type) {}

void SetTimeOfDayKeyframeInterpolation::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked || !isValidInterpolationType(mType)) return;

    auto const it = state.timeOfDayTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.timeOfDayTrack.keysByTick.end() || it->second.interpolationType == mType) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    it->second.interpolationType = mType;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimeOfDayKeyframeInterpolation::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimeOfDayKeyframeInterpolation::label() const {
    return "playback.refactorEditor.history.setTimeOfDayKeyframeInterpolation";
}

SetTimeOfDayKeyframeBezier::SetTimeOfDayKeyframeBezier(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2)
: mTick(tick),
  mCtrl1(ctrl1),
  mCtrl2(ctrl2) {}

void SetTimeOfDayKeyframeBezier::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked) return;

    auto const it = state.timeOfDayTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.timeOfDayTrack.keysByTick.end()) return;

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

void        SetTimeOfDayKeyframeBezier::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimeOfDayKeyframeBezier::label() const {
    return "playback.refactorEditor.history.setTimeOfDayKeyframeBezier";
}

SetTimeOfDayKeyframesInterpolation::SetTimeOfDayKeyframesInterpolation(
    std::vector<int>               ticks,
    model::CameraInterpolationType type
)
: mTicks(std::move(ticks)),
  mType(type) {}

void SetTimeOfDayKeyframesInterpolation::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked || !isValidInterpolationType(mType) || mTicks.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    auto before  = state;
    bool changed = false;
    for (int const tick : mTicks) {
        auto const it = state.timeOfDayTrack.keysByTick.find(tick);
        if (it == state.timeOfDayTrack.keysByTick.end() || it->second.interpolationType == mType) continue;
        it->second.interpolationType = mType;
        changed                      = true;
    }
    if (!changed) return;

    mBefore = std::move(before);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetTimeOfDayKeyframesInterpolation::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetTimeOfDayKeyframesInterpolation::label() const {
    return "playback.refactorEditor.history.setTimeOfDayKeyframesInterpolation";
}

void ClearTimeOfDayKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.timeOfDayTrack.locked || state.timeOfDayTrack.keysByTick.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.timeOfDayTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        ClearTimeOfDayKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string ClearTimeOfDayKeyframes::label() const { return "playback.refactorEditor.history.clearTimeOfDayKeyframes"; }

SetSceneDaytimeOverride::SetSceneDaytimeOverride(bool enabled, int ticks) : mEnabled(enabled), mTicks(ticks) {}

void SetSceneDaytimeOverride::execute(model::EditorStateExt& state) {
    mBefore.reset();
    int const clamped = model::clampTimeOfDayTicks(mTicks);
    if (state.daytimeOverrideEnabled == mEnabled && state.daytimeOverrideTicks == clamped) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                      = state;
    state.daytimeOverrideEnabled = mEnabled;
    state.daytimeOverrideTicks   = clamped;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSceneDaytimeOverride::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSceneDaytimeOverride::label() const { return "playback.refactorEditor.history.setSceneDaytimeOverride"; }

SetSceneWeatherOverride::SetSceneWeatherOverride(model::WeatherOverride weather) : mWeather(weather) {}

void SetSceneWeatherOverride::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!isValidWeatherOverride(mWeather) || state.weatherOverride == mWeather) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore               = state;
    state.weatherOverride = mWeather;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetSceneWeatherOverride::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetSceneWeatherOverride::label() const { return "playback.refactorEditor.history.setSceneWeatherOverride"; }

} // namespace playback::state::editing::command
