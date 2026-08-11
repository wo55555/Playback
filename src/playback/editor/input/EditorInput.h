#pragma once

#include <Windows.h>
#include <cstdint>

namespace playback::editor::input {

// Called every frame before ImGui::NewFrame
void syncFrame();

// Publishes whether a browser/editor frame currently owns keyboard input.
void               setUiVisible(bool visible);
[[nodiscard]] bool isUiVisible();

// Publishes whether holding the replay viewport currently routes input to MCBE.
void               setGameInputCaptured(bool captured);
[[nodiscard]] bool isGameInputCaptured();

// Routes a key event to MCBE or queues it for the next ImGui frame.
// Returns true when MCBE should consume the event.
[[nodiscard]] bool routeKeyEvent(uint32_t keyCode, bool down);

// Releases locally tracked keys when the application loses focus.
void releaseKeysForFocusLoss();

// Clears all routing state when the input hook is fully removed.
void resetInputState();

// Query: should MCBE consume mouse?
bool shouldMCBEConsumeMouse();

} // namespace playback::editor::input
