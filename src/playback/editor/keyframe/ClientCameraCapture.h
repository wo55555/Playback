#pragma once

#include "CameraTimelineEvaluator.h"

#include <optional>

namespace playback::editor::keyframe {

// Captures Bedrock's final client camera into the editor's pure render model.
// The local player's head pose is used only when the camera has not yet been
// initialized for the current replay frame.
[[nodiscard]] std::optional<CameraRenderState> captureClientCamera() noexcept;

} // namespace playback::editor::keyframe
