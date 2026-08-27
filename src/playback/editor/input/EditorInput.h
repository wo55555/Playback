#pragma once

#include <Windows.h>
#include <cstdint>

namespace playback::editor::input {

void syncFrame();

void               setUiVisible(bool visible);
[[nodiscard]] bool isUiVisible();

void               setGameInputCaptured(bool captured);
[[nodiscard]] bool isGameInputCaptured();

void               setUiKeyboardCaptured(bool captured);
[[nodiscard]] bool isUiKeyboardCaptured();

[[nodiscard]] bool routeKeyEvent(uint32_t keyCode, bool down, bool forceUi = false);

void releaseKeysForFocusLoss();

void resetInputState();

bool shouldMCBEConsumeMouse();

} // namespace playback::editor::input
