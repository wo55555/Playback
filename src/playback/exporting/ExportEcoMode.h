#pragma once

namespace playback::exporting {

// Main thread only. Turns Eco Mode off for the duration of an export and restores it afterwards.
void applyExportEcoMode(bool active) noexcept;

// Safe to call repeatedly; acts once, as soon as the client options exist.
void restoreEcoModeAfterCrash() noexcept;

} // namespace playback::exporting
