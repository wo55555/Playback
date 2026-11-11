#pragma once

#include "OfflineRenderClockHooks.h"

namespace playback::exporting {

// Ray tracing and Vibrant Visuals time their temporal effects (auto exposure, scrolling phases, shader time) on the
// wall clock. While an export is open those reads follow a virtual clock that only moves with the exported replay.
void beginOfflineRenderTime();
// Logs the session summary and keeps the clock continuous for the renderers after the export.
void endOfflineRenderTime();
// Main thread, before the render that consumes the sample; only the first render of a sample moves the clock.
void advanceOfflineRenderTime(OfflineRenderClockKind kind, float deltaTicks, bool firstRender);
// Restores the patched import; mod shutdown only.
void releaseOfflineRenderTimeHooks();

} // namespace playback::exporting
