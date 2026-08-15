#pragma once

#include "CameraRenderState.h"

#include <optional>

namespace playback::keyframe {

[[nodiscard]] std::optional<CameraRenderState> captureClientCamera() noexcept;

} // namespace playback::keyframe
