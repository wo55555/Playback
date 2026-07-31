#include "Clipboard.h"

namespace playback::refactor::video_editing {

void Clipboard::put(const std::vector<Clip>& clips, const std::vector<Transition>& transitions) {
    mClips       = clips;
    mTransitions = transitions;
}

std::vector<Clip> Clipboard::getClips() const {
    return mClips;
}

std::vector<Transition> Clipboard::getTransitions() const {
    return mTransitions;
}

void Clipboard::clear() {
    mClips.clear();
    mTransitions.clear();
}

bool Clipboard::hasContent() const {
    return !mClips.empty() || !mTransitions.empty();
}

} // namespace playback::refactor::video_editing