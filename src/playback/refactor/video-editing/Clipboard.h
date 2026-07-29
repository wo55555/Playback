#pragma once

#include "playback/refactor/editor/models/Track.h"

#include <vector>

namespace playback::refactor::video_editing {

using namespace playback::refactor::editor;

class Clipboard {
public:
    void put(const std::vector<Clip>& clips, const std::vector<Transition>& transitions);
    [[nodiscard]] std::vector<Clip>       getClips() const;
    [[nodiscard]] std::vector<Transition> getTransitions() const;
    void clear();

    [[nodiscard]] bool hasContent() const;

private:
    std::vector<Clip>       mClips;
    std::vector<Transition> mTransitions;
};

} // namespace playback::refactor::video_editing