#pragma once

namespace playback::editor::keyframe {
struct CameraTimelineSample;
}

namespace playback::editor::renderer {

// Applies the immutable preview/export sample at LevelRendererPlayer's native
// camera setup boundary and validates the resulting ViewRenderObject.
[[nodiscard]] bool hookCameraRender(bool enable);
[[nodiscard]] bool isCameraRenderInstalled();

} // namespace playback::editor::renderer
