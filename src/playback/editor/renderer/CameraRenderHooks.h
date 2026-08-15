#pragma once

namespace playback::editor::renderer {

[[nodiscard]] bool hookCameraRender(bool enable);
[[nodiscard]] bool isCameraRenderInstalled();

} // namespace playback::editor::renderer
