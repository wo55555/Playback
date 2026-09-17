#include "SelectionModel.h"

#include "EditorStateExt.h"

#include <algorithm>

namespace playback::state::editing::model {

void SelectionModel::select(Selection sel) { mSelection = std::move(sel); }

void SelectionModel::clear() { mSelection.reset(); }

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
                return {sel.trackId, sel.clipId};
            } else if constexpr (std::is_same_v<T, SelectedMarker>) {
                return {sel.markerId};
            } else if constexpr (std::is_same_v<T, SelectedTrack>) {
                return {sel.trackId};
            } else if constexpr (std::is_same_v<T, SelectedTransition>) {
                return {sel.transitionId};
            } else if constexpr (std::is_same_v<T, SelectedSequence>) {
                return {"sequence"};
            } else if constexpr (
                std::is_same_v<T, SelectedSequenceSegment> || std::is_same_v<T, SelectedWorldActorSegment>
            ) {
                return {sel.segmentId};
            } else if constexpr (std::is_same_v<T, SelectedWorldActor>) {
                return {"worldActor"};
            } else if constexpr (std::is_same_v<T, SelectedSubActor>) {
                return {sel.subActorId};
            } else if constexpr (std::is_same_v<T, SelectedCamera>) {
                return {sel.cameraId};
            }
            return {};
        },
        mSelection.value()
    );
}

void SelectionModel::pruneInvalid(EditorStateExt const& state) {
    if (!mSelection.has_value()) return;

    auto const camera = [&state](std::string const& id) {
        return std::ranges::any_of(state.cameras, [&id](auto const& entry) { return entry.id == id; });
    };

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
            } else if constexpr (std::is_same_v<T, SelectedWorldActorSegment>) {
                return std::ranges::any_of(state.worldActor.segments, [&](auto const& segment) {
                    return segment.id == sel.segmentId;
                });
            } else if constexpr (std::is_same_v<T, SelectedSubActor>) {
                return std::ranges::any_of(state.worldActor.subActors, [&](auto const& actor) {
                    return actor.id == sel.subActorId;
                });
            } else if constexpr (std::is_same_v<T, SelectedSequenceSegment>) {
                return std::ranges::any_of(state.sequence, [&](auto const& segment) {
                    return segment.id == sel.segmentId;
                });
            }
            return true;
        },
        mSelection.value()
    );
    if (!valid) mSelection.reset();
}

} // namespace playback::state::editing::model
