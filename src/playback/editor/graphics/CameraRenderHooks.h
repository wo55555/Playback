#pragma once

#include "playback/keyframe/CameraRenderState.h"

#include <optional>

namespace playback::editor::graphics {

[[nodiscard]] bool                                       hookCameraRender(bool enable);
[[nodiscard]] bool                                       isCameraRenderInstalled();
[[nodiscard]] std::optional<keyframe::CameraRenderState> currentRendererCameraState();

} // namespace playback::editor::graphics
