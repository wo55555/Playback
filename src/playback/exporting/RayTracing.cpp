#include "RayTracing.h"

namespace playback::exporting {

// The 26.10 SDK declares FrameBuilder without any state accessors, so ray tracing cannot be detected here.
bool rayTracingActive() noexcept { return false; }

} // namespace playback::exporting
