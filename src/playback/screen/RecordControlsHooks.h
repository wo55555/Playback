#pragma once

namespace playback::screen {

void hookRecordControls(bool enable);

// Whether the top-right recording indicator may show while a recording is active.
void               setRecordingIndicatorEnabled(bool enabled);
[[nodiscard]] bool isRecordingIndicatorEnabled();

} // namespace playback::screen
