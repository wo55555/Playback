#pragma once

#include <cstdint>

namespace playback::visuals {

// The replay observer is a spectator, so every container screen controller judges itself invalid on its first tick
// and leaves. While enabled, the controllers' validity check is forced to pass so the previewed screen stays up for
// the recorded interval, including while the replay is paused or stepped. Installs the hook on first use.
void setReplayContainerKeepOpen(bool enable);

// Removes the hook; called when the mod unloads.
void releaseReplayContainerKeepOpen();

// Number of validity checks overridden since the hook was installed.
[[nodiscard]] uint64_t replayContainerKeepOpenOverrides();

} // namespace playback::visuals
