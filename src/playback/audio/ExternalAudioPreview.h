#pragma once

#include "playback/state/editing/models/EditorStateExt.h"

namespace playback::audio::external {

void updatePreview(
    state::editing::model::EditorStateExt const& project,
    long double                                  replayTick,
    bool                                         playing,
    bool                                         seeking,
    bool                                         exporting,
    float                                        masterVolume,
    bool                                         masterMuted
);

void stopPreview();

void shutdownPreview();

} // namespace playback::audio::external
