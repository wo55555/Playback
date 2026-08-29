#pragma once

#include <cstdint>
#include <string>

namespace playback::editor::input {

enum class EditorKeybind : uint8_t {
    OpenExport,
    SaveProject,
    Undo,
    Redo,
    DeleteSelection,
    JumpStart,
    JumpEnd,
    SeekSecondLeft,
    SeekSecondRight,
    SeekTickLeft,
    SeekTickRight,
    PreviousEditPoint,
    NextEditPoint,
    DecreaseSpeed,
    IncreaseSpeed,
    AddKeyframe,
    ZoomInTimeline,
    ZoomOutTimeline,
    ResetTimelineZoom,
    ToggleViewportMaximized,
    NamedOnly,
};

class KeyMap {
public:
    static void initialize();

    // Query the current ImGui frame using exact modifier matching.
    static bool pressed(EditorKeybind binding, bool repeat = false);

    static std::string displayString(const std::string& actionName);
    static std::string displayString(EditorKeybind binding);
};

} // namespace playback::editor::input
