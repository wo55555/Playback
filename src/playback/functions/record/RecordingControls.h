#pragma once

#include "playback/Config.h"

#include <Windows.h>

namespace playback::functions {

class RecordingControls {
public:
    static RecordingControls& getInstance();

    void setGameHudVisible(bool visible);
    void resetPressedKeys();
    bool onKeyInput(UINT key, bool isDown, bool uiOwnsKeyboard = false);

    [[nodiscard]] bool isGameHudVisible() const;
    [[nodiscard]] bool isCapturingKey() const;
    void setKeyCapture(bool active);

private:
    RecordingControls() = default;
};

} // namespace playback::functions
