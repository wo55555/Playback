#include "SelectionModel.h"

#include <algorithm>

namespace playback::editor::editing {

void SelectionModel::select(const std::string& id, bool additive) {
    if (!additive) {
        clear();
    }

    // Try to add to clips first (most common case)
    if (std::find(mSelection.clipIds.begin(), mSelection.clipIds.end(), id)
        == mSelection.clipIds.end()) {
        mSelection.clipIds.push_back(id);
    }
}

void SelectionModel::clear() {
    mSelection.clipIds.clear();
    mSelection.trackIds.clear();
    mSelection.keyframeIds.clear();
    mSelection.transitionIds.clear();
}

bool SelectionModel::isSelected(const std::string& id) const {
    if (std::find(mSelection.clipIds.begin(), mSelection.clipIds.end(), id)
        != mSelection.clipIds.end()) return true;
    if (std::find(mSelection.trackIds.begin(), mSelection.trackIds.end(), id)
        != mSelection.trackIds.end()) return true;
    if (std::find(mSelection.keyframeIds.begin(), mSelection.keyframeIds.end(), id)
        != mSelection.keyframeIds.end()) return true;
    if (std::find(mSelection.transitionIds.begin(), mSelection.transitionIds.end(), id)
        != mSelection.transitionIds.end()) return true;
    return false;
}

Selection SelectionModel::snapshot() const {
    return mSelection;
}

void SelectionModel::setAnchor(int tick) {
    mSelection.anchorTick = tick;
}

bool SelectionModel::hasSelection() const {
    return !mSelection.clipIds.empty()
        || !mSelection.trackIds.empty()
        || !mSelection.keyframeIds.empty()
        || !mSelection.transitionIds.empty();
}

} // namespace playback::editor::editing