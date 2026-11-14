#pragma once

namespace playback::runtime {

// The window thread queues the close; the client thread drains replay cleanup before posting it again.
[[nodiscard]] bool deferWindowClose(void* window);
void               advanceWindowClose();

} // namespace playback::runtime
