#pragma once

namespace playback::exporting {

// 26.40 threaded a forcedGuiScale override through MinecraftGame::setUISizeAndScale,
// ClientInstance::_updateScreenSizeVariables and GuiData::calculateGuiScale. A non-zero value made
// calculateGuiScale skip the derivation and return the override verbatim; zero meant "derive it".
// 26.51 deleted the parameter from all three, so the game now always derives the GUI scale from the
// window and the override has no entry point left.
//
// This reinstates the override at the single place it took effect, so GuiData's screen size
// bookkeeping stays internally consistent: the game still runs its own derivation and still publishes
// the scale, only the returned value is substituted. The hook is inert until forceGuiScale arms it.
[[nodiscard]] bool hookGuiScale(bool enable);

// Applies to the next GUI scale calculation and is then consumed, mirroring the per-request lifetime
// the parameter had in 26.40. Non-positive values restore the game's own derivation.
void forceGuiScale(float scale);
void clearForcedGuiScale();

} // namespace playback::exporting
