#include "Clipboard.h"

namespace playback::state::editing {

void Clipboard::put(const std::vector<model::Clip>& clips, const std::vector<model::Transition>& transitions) {
    mClips       = clips;
    mTransitions = transitions;
}

std::vector<model::Clip> Clipboard::getClips() const { return mClips; }

std::vector<model::Transition> Clipboard::getTransitions() const { return mTransitions; }

void Clipboard::clear() {
    mClips.clear();
    mTransitions.clear();
}

bool Clipboard::hasContent() const { return !mClips.empty() || !mTransitions.empty(); }

} // namespace playback::state::editing
