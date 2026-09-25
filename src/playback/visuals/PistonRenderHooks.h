#pragma once

#include <cstdint>

namespace playback::visuals {

[[nodiscard]] bool hookPistonRender(bool enable);
[[nodiscard]] bool isPistonRenderInstalled() noexcept;

// Lets the piston diagnostics tag each row with the frame_%06d.png the export is currently producing. Pushed from the
// export boundary because a render hook has no way to learn the frame index on its own.
void setPistonDiagnosticsFrameIndex(uint64_t frameIndex) noexcept;
void clearPistonDiagnosticsFrameIndex() noexcept;

} // namespace playback::visuals
