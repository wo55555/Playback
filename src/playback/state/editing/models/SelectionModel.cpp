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
        [&](auto const& sel) -> bool {
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
            }
            return true;
        },
        mSelection.value()
    );
    if (!valid) mSelection.reset();
}

} // namespace playback::state::editing::model
