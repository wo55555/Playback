#pragma once

#include "CameraRenderState.h"
#include "playback/visuals/ReplaySampleTime.h"

#include <optional>

namespace playback::keyframe {

// The recorded player's eye view at `time` (entity time), or empty while it has no usable pose. Pure sampling: the
// render hooks refresh the sleep weight for the frame they draw.
[[nodiscard]] std::optional<CameraRenderState> sampleRecordedPlayerView(visuals::ReplaySampleTime const& time);

} // namespace playback::keyframe
