#pragma once

#include "playback/Config.h"
#include "playback/functions/record/Recorder.h"

#include <imgui.h>

namespace playback::editor::ui {

void drawRecordingStatusOverlay(
    functions::RecordingStatusSnapshot const& status,
    config::RecordingControlsConfig const&    config,
    ImVec2                                     displaySize
);

} // namespace playback::editor::ui
