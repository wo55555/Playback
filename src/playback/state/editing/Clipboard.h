#pragma once

#include "playback/state/editing/models/Track.h"

#include <vector>

namespace playback::state::editing {

class Clipboard {
public:
    void put(const std::vector<model::Clip>& clips, const std::vector<model::Transition>& transitions);
    [[nodiscard]] std::vector<model::Clip>       getClips() const;
    [[nodiscard]] std::vector<model::Transition> getTransitions() const;
    void                                         clear();

    [[nodiscard]] bool hasContent() const;

private:
    std::vector<model::Clip>       mClips;
    std::vector<model::Transition> mTransitions;
};

} // namespace playback::state::editing
