#pragma once

namespace playback::editor::keyframe {
struct CameraTimelineSample;
}

namespace playback::editor::renderer {

// Installs the final render-stage camera override at both GameRenderer's
// frame boundary and LevelRendererPlayer's native camera setup boundary.
// Preview and export therefore consume the same immutable timeline sample.
[[nodiscard]] bool hookCameraRender(bool enable);
[[nodiscard]] bool isCameraRenderInstalled();

} // namespace playback::editor::renderer
