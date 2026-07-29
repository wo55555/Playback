#pragma once

#include <Windows.h>

namespace playback::refactor::editor {

namespace InputHook {

// WndProc entry point (called from existing D3D12 hook)
// Returns true = message consumed, do NOT forward to MCBE
bool onWindowsMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Called every frame before ImGui::NewFrame
void syncFrame();

// Query: should MCBE consume keyboard?
bool shouldMCBEConsumeKeyboard();

// Query: should MCBE consume mouse?
bool shouldMCBEConsumeMouse();

} // namespace InputHook

} // namespace playback::refactor::editor