#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

namespace playback::editor::input {

enum class EditorKeybind : uint8_t {
    OpenExport,
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
    // Retained for hook-side callers that still route Windows virtual keys.
    static bool               matches(const std::string& actionName, WPARAM wParam);
    static bool               matches(EditorKeybind binding, WPARAM wParam);
    [[nodiscard]] static bool isEditorShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt);

    static void initialize();

    // Query the current ImGui frame using exact modifier matching.
    static bool pressed(EditorKeybind binding, bool repeat = false);

    static std::string displayString(const std::string& actionName);
    static std::string displayString(EditorKeybind binding);
};

} // namespace playback::editor::input
