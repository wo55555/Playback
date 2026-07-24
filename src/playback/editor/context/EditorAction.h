#pragma once

namespace playback::editor {

enum class EditorActionType {
    TogglePause,
    Seek,
    SkipToStart,
    SkipToEnd,
    DecreaseSpeed,
    IncreaseSpeed,
    StopReplay,
};

struct EditorAction {
    EditorActionType type{};
    int              tick{};
};

} // namespace playback::editor
