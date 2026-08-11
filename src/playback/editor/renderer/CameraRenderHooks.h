#pragma once

namespace playback::editor::renderer {

// Installs the final render-stage camera override.  The hook is independent
// from export clock control so preview and export share the same render path.
[[nodiscard]] bool hookCameraRender(bool enable);
[[nodiscard]] bool isCameraRenderInstalled();

} // namespace playback::editor::renderer
