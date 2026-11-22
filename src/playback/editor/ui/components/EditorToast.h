#pragma once

#include "imgui.h"

#include <string>

namespace playback::editor::ui {

// Transient capsule toast drawn over the viewport without stealing input focus.
class EditorToast {
public:
    void show(std::string message, float durationSeconds = 1.8f);
    void clear();
    void draw(ImVec2 viewportPos = {}, ImVec2 viewportSize = {}, float dt = -1.0f);

private:
    std::string mMessage;
    float       mElapsed{0.0f};
    float       mDuration{0.0f};
    bool        mVisible{false};
};

} // namespace playback::editor::ui
