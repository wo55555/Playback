#pragma once

namespace playback::exporting {

// Main thread only. Turns Eco Mode off for the duration of an export and restores it afterwards.
void applyExportEcoMode(bool active) noexcept;

// Main thread only, once the client has finished initializing. Undoes Eco Mode left off by an export that was killed.
void recoverEcoModeAfterInterruptedExport() noexcept;

} // namespace playback::exporting
