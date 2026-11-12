#pragma once

#include "OfflineRenderClockHooks.h"

namespace playback::exporting {

// Ray tracing and Vibrant Visuals time their temporal effects (auto exposure, scrolling phases, shader time) on the
// wall clock, and so do the screens with their interface animations, such as a chat message fading from the HUD. While
// an export is open the renderers read a virtual clock that only moves with the exported replay, and each screen is
// handed the replay time its frames added as the step of its animations.
void beginOfflineRenderTime();
// Logs the session summary and keeps the clock continuous for its readers after the export.
void endOfflineRenderTime();
// Main thread, before a render: moves the clock by the time that render adds. firstRender only feeds the summary.
void advanceOfflineRenderTime(OfflineRenderClockKind kind, float deltaTicks, bool firstRender);
// Main thread, before a frame the game draws for itself while an export is open, as it does through a dimension
// change: its screens are not part of the video, so their animations get the time that really passed.
void advanceOfflineRenderTimeForNativeFrame();
// Restores the patched import; mod shutdown only.
void releaseOfflineRenderTimeHooks();

} // namespace playback::exporting
