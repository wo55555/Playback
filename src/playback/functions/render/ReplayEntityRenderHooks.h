#pragma once

namespace playback::functions::render {

// Installs the export-only post-update hook that reapplies the sampled render
// pose after Bedrock's native interpolation system has run.
[[nodiscard]] bool hookReplayEntityRender(bool enable);
[[nodiscard]] bool isReplayEntityRenderInstalled();

} // namespace playback::functions::render
