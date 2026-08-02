#include "SelectionModel.h"

namespace playback::refactor::editor {

void SelectionModel::select(Selection sel) {
    mSelection = std::move(sel);
    // onSelectionChanged.emit(selectedIds());
}

void SelectionModel::clear() {
    mSelection.reset();
    // onSelectionChanged.emit({});
}

bool SelectionModel::hasSelection() const {
    return mSelection.has_value();
}

const Selection* SelectionModel::getSelection() const {
    if (!mSelection.has_value()) return nullptr;
    return &mSelection.value();
}

std::vector<std::string> SelectionModel::selectedIds() const {
    if (!mSelection.has_value()) return {};

    return std::visit([](const auto& sel) -> std::vector<std::string> {
        using T = std::decay_t<decltype(sel)>;
        if constexpr (std::is_same_v<T, SelectedKeyframe>) {
            return {sel.trackId, sel.keyframeId};
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
        } else if constexpr (std::is_same_v<T, SelectedSequenceSegment>
                             || std::is_same_v<T, SelectedWorldActorSegment>) {
            return {sel.segmentId};
        } else if constexpr (std::is_same_v<T, SelectedWorldActor>) {
            return {"worldActor"};
        } else if constexpr (std::is_same_v<T, SelectedSubActor>) {
            return {sel.subActorId};
        } else if constexpr (std::is_same_v<T, SelectedCamera>) {
            return {sel.cameraId};
        }
        return {};
    }, mSelection.value());
}

} // namespace playback::refactor::editor
