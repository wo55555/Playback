#include "WeatherCommands.h"

#include "playback/state/editing/ClipOps.h"

#include <algorithm>
#include <set>
#include <utility>

namespace playback::state::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

int clampTick(int tick, int totalTicks) noexcept { return std::clamp(tick, 0, std::max(0, totalTicks)); }

bool isValidWeatherOverride(model::WeatherOverride weather) noexcept {
    return static_cast<int>(weather) >= 0
        && static_cast<int>(weather) <= static_cast<int>(model::WeatherOverride::Thunder);
}

} // namespace

void AddWeatherTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.isVisible()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                    = state;
    state.weatherTrack.present = true;
    state.weatherTrack.enabled = true;
    state.weatherTrack.locked  = false;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddWeatherTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddWeatherTrack::label() const { return "playback.refactorEditor.history.addWeatherTrack"; }

void RemoveWeatherTrack::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!state.weatherTrack.isVisible() || state.weatherTrack.locked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                    = state;
    state.weatherTrack.present = false;
    state.weatherTrack.enabled = true;
    state.weatherTrack.locked  = false;
    state.weatherTrack.keysByTick.clear();
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveWeatherTrack::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveWeatherTrack::label() const { return "playback.refactorEditor.history.removeWeatherTrack"; }

SetWeatherTrackEnabled::SetWeatherTrackEnabled(bool enabled) : mEnabled(enabled) {}

void SetWeatherTrackEnabled::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.enabled == mEnabled) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                    = state;
    state.weatherTrack.enabled = mEnabled;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetWeatherTrackEnabled::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetWeatherTrackEnabled::label() const { return "playback.refactorEditor.history.setWeatherTrackEnabled"; }

SetWeatherTrackLocked::SetWeatherTrackLocked(bool locked) : mLocked(locked) {}

void SetWeatherTrackLocked::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked == mLocked) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                   = state;
    state.weatherTrack.locked = mLocked;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetWeatherTrackLocked::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetWeatherTrackLocked::label() const { return "playback.refactorEditor.history.setWeatherTrackLocked"; }

AddWeatherKeyframe::AddWeatherKeyframe(int tick, model::WeatherOverride weather) : mTick(tick), mWeather(weather) {}

void AddWeatherKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    mReplaced = false;
    if (state.weatherTrack.locked || !isValidWeatherOverride(mWeather)) return;

    int const  tick     = clampTick(mTick, state.totalTicks);
    auto const existing = state.weatherTrack.keysByTick.find(tick);
    if (existing != state.weatherTrack.keysByTick.end()) {
        if (existing->second.weather == mWeather && state.weatherTrack.present) return;
        mReplaced = true;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore                    = state;
    state.weatherTrack.present = true;
    state.weatherTrack.keysByTick.insert_or_assign(tick, model::WeatherKeyframe{mWeather});
    ClipOps::syncActiveSceneFromWorking(state);
}

void        AddWeatherKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddWeatherKeyframe::label() const {
    return mReplaced ? "playback.refactorEditor.history.updateWeatherKeyframe"
                     : "playback.refactorEditor.history.addWeatherKeyframe";
}

RemoveWeatherKeyframe::RemoveWeatherKeyframe(int tick) : mTick(tick) {}

void RemoveWeatherKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked) return;

    auto const it = state.weatherTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.weatherTrack.keysByTick.end()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;
    state.weatherTrack.keysByTick.erase(it);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveWeatherKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveWeatherKeyframe::label() const { return "playback.refactorEditor.history.removeWeatherKeyframe"; }

RemoveWeatherKeyframes::RemoveWeatherKeyframes(std::vector<int> ticks) : mTicks(std::move(ticks)) {}

void RemoveWeatherKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked || mTicks.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    auto before  = state;
    bool changed = false;
    for (int const tick : mTicks) {
        changed |= state.weatherTrack.keysByTick.erase(tick) > 0;
    }
    if (!changed) return;

    mBefore = std::move(before);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        RemoveWeatherKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RemoveWeatherKeyframes::label() const { return "playback.refactorEditor.history.removeWeatherKeyframes"; }

MoveWeatherKeyframe::MoveWeatherKeyframe(int fromTick, int toTick) : mFromTick(fromTick), mToTick(toTick) {}

void MoveWeatherKeyframe::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked) return;

    int const  fromTick = clampTick(mFromTick, state.totalTicks);
    int const  toTick   = clampTick(mToTick, state.totalTicks);
    auto const source   = state.weatherTrack.keysByTick.find(fromTick);
    if (source == state.weatherTrack.keysByTick.end() || fromTick == toTick
        || state.weatherTrack.keysByTick.contains(toTick)) {
        return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore  = state;
    auto key = std::move(source->second);
    state.weatherTrack.keysByTick.erase(source);
    state.weatherTrack.keysByTick.emplace(toTick, std::move(key));
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveWeatherKeyframe::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveWeatherKeyframe::label() const { return "playback.refactorEditor.history.moveWeatherKeyframe"; }

MoveWeatherKeyframes::MoveWeatherKeyframes(std::vector<int> ticks, int tickDelta)
: mTicks(std::move(ticks)),
  mTickDelta(tickDelta) {}

void MoveWeatherKeyframes::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked || mTickDelta == 0 || mTicks.empty()) return;

    std::set<int> moving(mTicks.begin(), mTicks.end());
    for (int const tick : moving) {
        int const target = tick + mTickDelta;
        if (!state.weatherTrack.keysByTick.contains(tick) || target < 0 || target > state.totalTicks) return;
        if (state.weatherTrack.keysByTick.contains(target) && !moving.contains(target)) return;
    }

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore = state;

    std::vector<std::pair<int, model::WeatherKeyframe>> lifted;
    lifted.reserve(moving.size());
    for (int const tick : moving) {
        auto node = state.weatherTrack.keysByTick.extract(tick);
        lifted.emplace_back(tick + mTickDelta, std::move(node.mapped()));
    }
    for (auto& [tick, key] : lifted) {
        state.weatherTrack.keysByTick.emplace(tick, std::move(key));
    }
    ClipOps::syncActiveSceneFromWorking(state);
}

void        MoveWeatherKeyframes::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveWeatherKeyframes::label() const { return "playback.refactorEditor.history.moveWeatherKeyframes"; }

SetWeatherKeyframeValue::SetWeatherKeyframeValue(int tick, model::WeatherOverride weather)
: mTick(tick),
  mWeather(weather) {}

void SetWeatherKeyframeValue::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked || !isValidWeatherOverride(mWeather)) return;

    auto const it = state.weatherTrack.keysByTick.find(clampTick(mTick, state.totalTicks));
    if (it == state.weatherTrack.keysByTick.end() || it->second.weather == mWeather) return;

    ClipOps::syncActiveSceneFromWorking(state);
    mBefore            = state;
    it->second.weather = mWeather;
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetWeatherKeyframeValue::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetWeatherKeyframeValue::label() const { return "playback.refactorEditor.history.setWeatherKeyframeValue"; }

SetWeatherKeyframesValue::SetWeatherKeyframesValue(std::vector<int> ticks, model::WeatherOverride weather)
: mTicks(std::move(ticks)),
  mWeather(weather) {}

void SetWeatherKeyframesValue::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (state.weatherTrack.locked || !isValidWeatherOverride(mWeather) || mTicks.empty()) return;

    ClipOps::syncActiveSceneFromWorking(state);
    auto before  = state;
    bool changed = false;
    for (int const tick : mTicks) {
        auto const it = state.weatherTrack.keysByTick.find(tick);
        if (it == state.weatherTrack.keysByTick.end() || it->second.weather == mWeather) continue;
        it->second.weather = mWeather;
        changed            = true;
    }
    if (!changed) return;

    mBefore = std::move(before);
    ClipOps::syncActiveSceneFromWorking(state);
}

void        SetWeatherKeyframesValue::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SetWeatherKeyframesValue::label() const {
    return "playback.refactorEditor.history.setWeatherKeyframesValue";
}

} // namespace playback::state::editing::command
