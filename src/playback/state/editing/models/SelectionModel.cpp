#include "SelectionModel.h"

#include "EditorStateExt.h"

#include <algorithm>

namespace playback::state::editing::model {

void SelectionModel::select(Selection sel) {
    mSelection = std::move(sel);
    mKeyframes.clear();
    if (auto const* key = std::get_if<SelectedKeyframe>(&*mSelection)) mKeyframes.push_back(*key);
}

void SelectionModel::clear() {
    mSelection.reset();
    mKeyframes.clear();
}

void SelectionModel::selectSpeedTrack() { select(SelectedSpeedTrack{}); }

void SelectionModel::selectSpeedKeyframe(int tick) { select(SelectedSpeedKeyframe{tick}); }

bool SelectionModel::isSpeedTrackSelected() const { return getAs<SelectedSpeedTrack>() != nullptr; }

std::optional<int> SelectionModel::selectedSpeedKeyframe() const {
    if (auto const* key = getAs<SelectedSpeedKeyframe>()) return key->tick;
    return std::nullopt;
}

void SelectionModel::selectFreezeTrack() { select(SelectedFreezeTrack{}); }

void SelectionModel::selectFreezeKeyframe(int tick) { select(SelectedFreezeKeyframe{tick}); }

bool SelectionModel::isFreezeTrackSelected() const { return getAs<SelectedFreezeTrack>() != nullptr; }

std::optional<int> SelectionModel::selectedFreezeKeyframe() const {
    if (auto const* key = getAs<SelectedFreezeKeyframe>()) return key->tick;
    return std::nullopt;
}

void SelectionModel::selectTimelapseTrack() { select(SelectedTimelapseTrack{}); }

void SelectionModel::selectTimelapseKeyframe(int tick) { select(SelectedTimelapseKeyframe{tick}); }

bool SelectionModel::isTimelapseTrackSelected() const { return getAs<SelectedTimelapseTrack>() != nullptr; }

std::optional<int> SelectionModel::selectedTimelapseKeyframe() const {
    if (auto const* key = getAs<SelectedTimelapseKeyframe>()) return key->tick;
    return std::nullopt;
}

void SelectionModel::selectTimeOfDayTrack() { select(SelectedTimeOfDayTrack{}); }

void SelectionModel::selectTimeOfDayKeyframe(int tick) { select(SelectedTimeOfDayKeyframe{tick}); }

void SelectionModel::selectTimeOfDayKeyframes(std::vector<int> ticks) {
    std::ranges::sort(ticks);
    auto [uniqueEnd, end] = std::ranges::unique(ticks);
    ticks.erase(uniqueEnd, end);
    if (ticks.empty()) {
        clear();
        return;
    }
    select(SelectedTimeOfDayKeyframes{std::move(ticks)});
}

bool SelectionModel::isTimeOfDayTrackSelected() const { return getAs<SelectedTimeOfDayTrack>() != nullptr; }

std::optional<int> SelectionModel::selectedTimeOfDayKeyframe() const {
    if (auto const* key = getAs<SelectedTimeOfDayKeyframe>()) return key->tick;
    if (auto const* keys = getAs<SelectedTimeOfDayKeyframes>(); keys && !keys->ticks.empty()) {
        return keys->ticks.back();
    }
    return std::nullopt;
}

std::vector<int> SelectionModel::selectedTimeOfDayKeyframes() const {
    if (auto const* key = getAs<SelectedTimeOfDayKeyframe>()) return {key->tick};
    if (auto const* keys = getAs<SelectedTimeOfDayKeyframes>()) return keys->ticks;
    return {};
}

bool SelectionModel::isTimeOfDayKeyframeSelected(int tick) const {
    if (auto const* key = getAs<SelectedTimeOfDayKeyframe>()) return key->tick == tick;
    if (auto const* keys = getAs<SelectedTimeOfDayKeyframes>()) {
        return std::ranges::find(keys->ticks, tick) != keys->ticks.end();
    }
    return false;
}

void SelectionModel::selectWeatherTrack() { select(SelectedWeatherTrack{}); }

void SelectionModel::selectWeatherKeyframe(int tick) { select(SelectedWeatherKeyframe{tick}); }

void SelectionModel::selectWeatherKeyframes(std::vector<int> ticks) {
    std::ranges::sort(ticks);
    auto [uniqueEnd, end] = std::ranges::unique(ticks);
    ticks.erase(uniqueEnd, end);
    if (ticks.empty()) {
        clear();
        return;
    }
    select(SelectedWeatherKeyframes{std::move(ticks)});
}

bool SelectionModel::isWeatherTrackSelected() const { return getAs<SelectedWeatherTrack>() != nullptr; }

std::optional<int> SelectionModel::selectedWeatherKeyframe() const {
    if (auto const* key = getAs<SelectedWeatherKeyframe>()) return key->tick;
    if (auto const* keys = getAs<SelectedWeatherKeyframes>(); keys && !keys->ticks.empty()) {
        return keys->ticks.back();
    }
    return std::nullopt;
}

std::vector<int> SelectionModel::selectedWeatherKeyframes() const {
    if (auto const* key = getAs<SelectedWeatherKeyframe>()) return {key->tick};
    if (auto const* keys = getAs<SelectedWeatherKeyframes>()) return keys->ticks;
    return {};
}

bool SelectionModel::isWeatherKeyframeSelected(int tick) const {
    if (auto const* key = getAs<SelectedWeatherKeyframe>()) return key->tick == tick;
    if (auto const* keys = getAs<SelectedWeatherKeyframes>()) {
        return std::ranges::find(keys->ticks, tick) != keys->ticks.end();
    }
    return false;
}

void SelectionModel::selectAudioTrack(std::string trackId) { select(SelectedAudioTrack{std::move(trackId)}); }

void SelectionModel::selectAudioClip(std::string trackId, std::string clipId) {
    select(SelectedAudioClip{std::move(trackId), std::move(clipId)});
}

std::optional<std::string> SelectionModel::selectedAudioTrackId() const {
    if (auto const* track = getAs<SelectedAudioTrack>()) return track->trackId;
    return std::nullopt;
}

std::optional<SelectedAudioClip> SelectionModel::selectedAudioClip() const {
    if (auto const* clip = getAs<SelectedAudioClip>()) return *clip;
    return std::nullopt;
}

void SelectionModel::setKeyframes(std::vector<SelectedKeyframe> keys) {
    if (keys.empty()) {
        clear();
        return;
    }
    mSelection = keys.back();
    mKeyframes = std::move(keys);
}

void SelectionModel::toggleKeyframe(SelectedKeyframe key) {
    auto const it = std::ranges::find_if(mKeyframes, [&](auto const& entry) {
        return entry.trackId == key.trackId && entry.tick == key.tick;
    });
    if (it == mKeyframes.end()) {
        mKeyframes.push_back(key);
        mSelection = std::move(key);
        return;
    }
    mKeyframes.erase(it);
    if (mKeyframes.empty()) {
        mSelection.reset();
        return;
    }
    mSelection = mKeyframes.back();
}

bool SelectionModel::isKeyframeSelected(std::string const& trackId, int tick) const {
    return std::ranges::any_of(mKeyframes, [&](auto const& entry) {
        return entry.tick == tick && entry.trackId == trackId;
    });
}

bool SelectionModel::hasSelection() const { return mSelection.has_value(); }

const Selection* SelectionModel::getSelection() const {
    if (!mSelection.has_value()) return nullptr;
    return &mSelection.value();
}

std::vector<std::string> SelectionModel::selectedIds() const {
    if (!mSelection.has_value()) return {};

    return std::visit(
        [](const auto& sel) -> std::vector<std::string> {
            using T = std::decay_t<decltype(sel)>;
            if constexpr (std::is_same_v<T, SelectedKeyframe>) {
                return {sel.trackId, std::to_string(sel.tick)};
            } else if constexpr (std::is_same_v<T, SelectedClip>) {
                return {sel.clipId};
            } else if constexpr (std::is_same_v<T, SelectedMarker>) {
                return {sel.markerId};
            } else if constexpr (std::is_same_v<T, SelectedTransition>) {
                return {sel.transitionId};
            } else if constexpr (std::is_same_v<T, SelectedCamera>) {
                return {sel.cameraId};
            } else if constexpr (std::is_same_v<T, SelectedSpeedTrack>) {
                return {"speed_track"};
            } else if constexpr (std::is_same_v<T, SelectedSpeedKeyframe>) {
                return {"speed_track", std::to_string(sel.tick)};
            } else if constexpr (std::is_same_v<T, SelectedFreezeTrack>) {
                return {"freeze_track"};
            } else if constexpr (std::is_same_v<T, SelectedFreezeKeyframe>) {
                return {"freeze_track", std::to_string(sel.tick)};
            } else if constexpr (std::is_same_v<T, SelectedTimelapseTrack>) {
                return {"timelapse_track"};
            } else if constexpr (std::is_same_v<T, SelectedTimelapseKeyframe>) {
                return {"timelapse_track", std::to_string(sel.tick)};
            } else if constexpr (std::is_same_v<T, SelectedTimeOfDayTrack>) {
                return {"time_of_day_track"};
            } else if constexpr (std::is_same_v<T, SelectedTimeOfDayKeyframe>) {
                return {"time_of_day_track", std::to_string(sel.tick)};
            } else if constexpr (std::is_same_v<T, SelectedTimeOfDayKeyframes>) {
                std::vector<std::string> ids;
                ids.reserve(sel.ticks.size() + 1);
                ids.emplace_back("time_of_day_track");
                for (int tick : sel.ticks) ids.push_back(std::to_string(tick));
                return ids;
            } else if constexpr (std::is_same_v<T, SelectedWeatherTrack>) {
                return {"weather_track"};
            } else if constexpr (std::is_same_v<T, SelectedWeatherKeyframe>) {
                return {"weather_track", std::to_string(sel.tick)};
            } else if constexpr (std::is_same_v<T, SelectedWeatherKeyframes>) {
                std::vector<std::string> ids;
                ids.reserve(sel.ticks.size() + 1);
                ids.emplace_back("weather_track");
                for (int tick : sel.ticks) ids.push_back(std::to_string(tick));
                return ids;
            } else if constexpr (std::is_same_v<T, SelectedAudioTrack>) {
                return {sel.trackId};
            } else if constexpr (std::is_same_v<T, SelectedAudioClip>) {
                return {sel.trackId, sel.clipId};
            }
            return {};
        },
        mSelection.value()
    );
}

void SelectionModel::pruneInvalid(EditorStateExt const& state) {
    if (!mLastSceneId.empty() && mLastSceneId != state.activeSceneId) {
        clear();
        mLastSceneId = state.activeSceneId;
        return;
    }
    if (mLastSceneId.empty()) mLastSceneId = state.activeSceneId;

    if (!mSelection.has_value()) return;

    auto const camera = [&state](std::string const& id) {
        return std::ranges::any_of(state.cameras, [&id](auto const& entry) { return entry.id == id; });
    };

    if (!mKeyframes.empty()) {
        std::erase_if(mKeyframes, [&state](SelectedKeyframe const& key) {
            auto const it = std::ranges::find(state.cameras, key.trackId, &CameraEntity::id);
            return it == state.cameras.end() || !it->keysByTick.contains(key.tick);
        });
        if (mKeyframes.empty()) {
            mSelection.reset();
            return;
        }
        // Keep the primary pointing at a surviving member of the set.
        if (auto const* primary = std::get_if<SelectedKeyframe>(&*mSelection);
            primary && !isKeyframeSelected(primary->trackId, primary->tick)) {
            mSelection = mKeyframes.back();
        }
        return;
    }

    bool const valid = std::visit(
        [&](auto& sel) -> bool {
            using T = std::decay_t<decltype(sel)>;
            if constexpr (std::is_same_v<T, SelectedCamera>) {
                return camera(sel.cameraId);
            } else if constexpr (std::is_same_v<T, SelectedKeyframe>) {
                auto const it = std::ranges::find(state.cameras, sel.trackId, &CameraEntity::id);
                return it != state.cameras.end() && it->keysByTick.contains(sel.tick);
            } else if constexpr (std::is_same_v<T, SelectedMarker>) {
                return std::ranges::any_of(state.markers, [&](auto const& marker) {
                    return marker.id == sel.markerId;
                });
            } else if constexpr (std::is_same_v<T, SelectedClip>) {
                return std::ranges::any_of(state.cameras, [&](auto const& cam) {
                    return std::ranges::any_of(cam.clips, [&](auto const& clip) { return clip.id == sel.clipId; });
                });
            } else if constexpr (std::is_same_v<T, SelectedTransition>) {
                return std::ranges::any_of(state.transitions, [&](auto const& transition) {
                    return transition.id == sel.transitionId;
                });
            } else if constexpr (std::is_same_v<T, SelectedSpeedTrack>) {
                return state.speedTrack.isVisible();
            } else if constexpr (std::is_same_v<T, SelectedSpeedKeyframe>) {
                return state.speedTrack.keysByTick.contains(sel.tick);
            } else if constexpr (std::is_same_v<T, SelectedFreezeTrack>) {
                return state.freezeTrack.isVisible();
            } else if constexpr (std::is_same_v<T, SelectedFreezeKeyframe>) {
                return state.freezeTrack.keysByTick.contains(sel.tick);
            } else if constexpr (std::is_same_v<T, SelectedTimelapseTrack>) {
                return state.timelapseTrack.isVisible();
            } else if constexpr (std::is_same_v<T, SelectedTimelapseKeyframe>) {
                return state.timelapseTrack.keysByTick.contains(sel.tick);
            } else if constexpr (std::is_same_v<T, SelectedTimeOfDayTrack>) {
                return state.timeOfDayTrack.isVisible();
            } else if constexpr (std::is_same_v<T, SelectedTimeOfDayKeyframe>) {
                return state.timeOfDayTrack.keysByTick.contains(sel.tick);
            } else if constexpr (std::is_same_v<T, SelectedTimeOfDayKeyframes>) {
                std::erase_if(sel.ticks, [&](int tick) { return !state.timeOfDayTrack.keysByTick.contains(tick); });
                return !sel.ticks.empty();
            } else if constexpr (std::is_same_v<T, SelectedWeatherTrack>) {
                return state.weatherTrack.isVisible();
            } else if constexpr (std::is_same_v<T, SelectedWeatherKeyframe>) {
                return state.weatherTrack.keysByTick.contains(sel.tick);
            } else if constexpr (std::is_same_v<T, SelectedWeatherKeyframes>) {
                std::erase_if(sel.ticks, [&](int tick) { return !state.weatherTrack.keysByTick.contains(tick); });
                return !sel.ticks.empty();
            } else if constexpr (std::is_same_v<T, SelectedAudioTrack>) {
                return std::ranges::any_of(state.audioTracks, [&](auto const& track) {
                    return track.id == sel.trackId;
                });
            } else if constexpr (std::is_same_v<T, SelectedAudioClip>) {
                auto const it = std::ranges::find(state.audioTracks, sel.trackId, &AudioTrack::id);
                return it != state.audioTracks.end()
                    && std::ranges::any_of(it->clips, [&](auto const& clip) { return clip.id == sel.clipId; });
            }
            return true;
        },
        mSelection.value()
    );
    if (!valid) mSelection.reset();
}

} // namespace playback::state::editing::model
