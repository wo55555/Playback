#pragma once

namespace playback::exporting {

// Only a ray-traced pipeline accumulates across frames, so raster shaders need no convergence passes at all.
[[nodiscard]] bool rayTracingActive() noexcept;

} // namespace playback::exporting
