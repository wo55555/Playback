#pragma once

namespace playback::visuals {

// While the export view is first-person, the observer cannot show hands (a spectator's first-person model is hidden),
// so the native hand pass runs on the recorded player instead.
void hookReplayFirstPersonHands(bool enable);

// True inside the swapped hand pass, where the recorded player's own model must not be hidden.
[[nodiscard]] bool isReplayFirstPersonHandPass();

} // namespace playback::visuals
