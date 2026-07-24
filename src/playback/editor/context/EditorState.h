#pragma once

namespace playback::editor {

struct EditorState {
    bool  replayVisible{};
    bool  hudVisible{};
    bool  paused{};
    float playbackSpeed{1.0f};
    int   currentTick{};
    int   totalTicks{};
};

} // namespace playback::editor
