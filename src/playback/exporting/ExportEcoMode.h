#pragma once

namespace playback::exporting {

// Main thread only. Turns Eco Mode off for the duration of an export and restores it afterwards.
void applyExportEcoMode(bool active) noexcept;

} // namespace playback::exporting
