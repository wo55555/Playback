#pragma once

#include <Windows.h>
#include <cstdint>

namespace playback::refactor::editor {

namespace InputHook {

// Called every frame before ImGui::NewFrame
void syncFrame();

// Called from MCBE key event handler (thread-safe, no ImGui context needed)
// Stores key state for later forwarding to ImGui during syncFrame()
void onKeyEvent(uint32_t keyCode, bool down);

// Query: should MCBE consume keyboard?
bool shouldMCBEConsumeKeyboard();

// Query: should MCBE consume mouse?
bool shouldMCBEConsumeMouse();

} // namespace InputHook

} // namespace playback::refactor::editor