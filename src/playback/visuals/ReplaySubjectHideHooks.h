#pragma once

namespace playback::visuals {

// Skips drawing the hidden replay subject (body, hands, held item) while its render scope is active; the invisible
// flags alone only remove the head. Installed for the lifetime of an offline render.
void hookReplaySubjectHide(bool enable);

} // namespace playback::visuals
