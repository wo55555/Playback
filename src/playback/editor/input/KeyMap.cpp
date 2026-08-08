#include "playback/editor/input/KeyMap.h"

#include "imgui.h"

#include <Windows.h>

#include <string_view>

namespace playback::editor::input {

namespace {

struct KeyBinding {
    EditorKeybind    id;
    std::string_view action;
    UINT             vkey;
    bool             ctrl;
    bool             shift;
    bool             alt;
};

constexpr KeyBinding kBindings[] = {
    // File
    {EditorKeybind::NamedOnly,                "playback.editor.openReplay",      'O',         true,  false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.saveProject",     'S',         true,  false, false},
    {EditorKeybind::OpenExport,               "playback.editor.export",          'E',         true,  false, false},
    // Edit
    {EditorKeybind::Undo,                     "playback.editor.undo",            'Z',         true,  false, false},
    {EditorKeybind::Redo,                     "playback.editor.redo",            'Y',         true,  false, false},
    {EditorKeybind::Redo,                     "playback.editor.redo",            'Z',         true,  true,  false},
    {EditorKeybind::DeleteSelection,          "playback.editor.delete",          VK_DELETE,   false, false, false},
    {EditorKeybind::DeleteSelection,          "playback.editor.delete",          VK_BACK,     false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.selectAll",       'A',         true,  false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.cancel",          VK_ESCAPE,   false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.cut",             'X',         true,  false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.copy",            'C',         true,  false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.paste",           'V',         true,  false, false},
    // Playback
    {EditorKeybind::PlayPause,                "playback.editor.playPause",       VK_SPACE,    false, false, false},
    {EditorKeybind::JumpStart,                "playback.editor.jumpStart",       VK_HOME,     false, false, false},
    {EditorKeybind::JumpEnd,                  "playback.editor.jumpEnd",         VK_END,      false, false, false},
    {EditorKeybind::SeekSecondLeft,           "playback.editor.stepSecondLeft",  VK_LEFT,     false, false, false},
    {EditorKeybind::SeekSecondRight,          "playback.editor.stepSecondRight", VK_RIGHT,    false, false, false},
    {EditorKeybind::SeekTickLeft,             "playback.editor.stepLeft",        VK_LEFT,     true,  false, false},
    {EditorKeybind::SeekTickRight,            "playback.editor.stepRight",       VK_RIGHT,    true,  false, false},
    {EditorKeybind::PreviousEditPoint,        "playback.editor.previousPoint",   VK_DOWN,     false, false, false},
    {EditorKeybind::NextEditPoint,            "playback.editor.nextPoint",       VK_UP,       false, false, false},
    {EditorKeybind::DecreaseSpeed,            "playback.editor.decreaseSpeed",   VK_OEM_MINUS,false, false, false},
    {EditorKeybind::IncreaseSpeed,            "playback.editor.increaseSpeed",   VK_OEM_PLUS, false, false, false},
    // Timeline editing
    {EditorKeybind::AddKeyframe,              "playback.editor.addKeyframe",     'K',         false, false, false},
    {EditorKeybind::SplitAtPlayhead,          "playback.editor.splitClip",       'K',         true,  false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.addCameraTrack",  'N',         true,  true,  false},
    {EditorKeybind::NamedOnly,                "playback.editor.camera1",          '1',         false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.camera2",          '2',         false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.camera3",          '3',         false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.setInPoint",        'I',         false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.setOutPoint",       'O',         false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.insertMarker",      'M',         false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.jumpPrevMarker",    VK_OEM_4,    false, false, false},
    {EditorKeybind::NamedOnly,                "playback.editor.jumpNextMarker",    VK_OEM_6,    false, false, false},
    {EditorKeybind::ZoomInTimeline,           "playback.editor.zoomInTimeline",  VK_OEM_PLUS, true,  false, false},
    {EditorKeybind::ZoomOutTimeline,          "playback.editor.zoomOutTimeline", VK_OEM_MINUS,true,  false, false},
    {EditorKeybind::ResetTimelineZoom,        "playback.editor.resetZoom",       '0',         true,  false, false},
    {EditorKeybind::ToggleViewportMaximized,  "playback.editor.maximizeViewport", 'F',         true,  true,  false},
    {EditorKeybind::NamedOnly,                "playback.editor.toggleUI",          VK_F1,       false, false, false},
};

[[nodiscard]] bool modifiersMatch(KeyBinding const& binding, bool ctrl, bool shift, bool alt) {
    return binding.ctrl == ctrl && binding.shift == shift && binding.alt == alt;
}

[[nodiscard]] bool currentWindowsModifier(UINT key) { return (GetKeyState(static_cast<int>(key)) & 0x8000) != 0; }

[[nodiscard]] ImGuiKey vkeyToImGuiKey(UINT vkey) {
    if (vkey >= 'A' && vkey <= 'Z') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_A) + static_cast<int>(vkey - 'A'));
    }
    if (vkey >= '0' && vkey <= '9') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_0) + static_cast<int>(vkey - '0'));
    }
    switch (vkey) {
    case VK_BACK:
        return ImGuiKey_Backspace;
    case VK_SPACE:
        return ImGuiKey_Space;
    case VK_HOME:
        return ImGuiKey_Home;
    case VK_END:
        return ImGuiKey_End;
    case VK_LEFT:
        return ImGuiKey_LeftArrow;
    case VK_RIGHT:
        return ImGuiKey_RightArrow;
    case VK_UP:
        return ImGuiKey_UpArrow;
    case VK_DOWN:
        return ImGuiKey_DownArrow;
    case VK_DELETE:
        return ImGuiKey_Delete;
    case VK_ESCAPE:
        return ImGuiKey_Escape;
    case VK_OEM_PLUS:
        return ImGuiKey_Equal;
    case VK_OEM_MINUS:
        return ImGuiKey_Minus;
    case VK_OEM_4:
        return ImGuiKey_LeftBracket;
    case VK_OEM_6:
        return ImGuiKey_RightBracket;
    case VK_F1:
        return ImGuiKey_F1;
    default:
        return ImGuiKey_None;
    }
}

[[nodiscard]] std::string keyDisplayName(UINT vkey) {
    if ((vkey >= 'A' && vkey <= 'Z') || (vkey >= '0' && vkey <= '9')) {
        return std::string(1, static_cast<char>(vkey));
    }
    switch (vkey) {
    case VK_BACK:
        return "Backspace";
    case VK_SPACE:
        return "Space";
    case VK_HOME:
        return "Home";
    case VK_END:
        return "End";
    case VK_LEFT:
        return "Left";
    case VK_RIGHT:
        return "Right";
    case VK_UP:
        return "Up";
    case VK_DOWN:
        return "Down";
    case VK_DELETE:
        return "Delete";
    case VK_ESCAPE:
        return "Esc";
    case VK_OEM_PLUS:
        return "=";
    case VK_OEM_MINUS:
        return "-";
    case VK_OEM_4:
        return "[";
    case VK_OEM_6:
        return "]";
    case VK_F1:
        return "F1";
    default:
        return {};
    }
}

template <class Predicate>
[[nodiscard]] bool anyBinding(Predicate&& predicate) {
    for (auto const& binding : kBindings) {
        if (predicate(binding)) return true;
    }
    return false;
}

template <class Predicate>
[[nodiscard]] std::string bindingDisplayString(Predicate&& predicate) {
    std::string result;
    for (auto const& binding : kBindings) {
        if (!predicate(binding)) continue;
        std::string display;
        if (binding.ctrl) display += "Ctrl+";
        if (binding.shift) display += "Shift+";
        if (binding.alt) display += "Alt+";
        display += keyDisplayName(binding.vkey);
        if (display.empty()) continue;
        if (!result.empty()) result += " / ";
        result += display;
    }
    return result;
}

} // namespace

bool KeyMap::matches(const std::string& actionName, WPARAM wParam) {
    bool const ctrl  = currentWindowsModifier(VK_CONTROL);
    bool const shift = currentWindowsModifier(VK_SHIFT);
    bool const alt   = currentWindowsModifier(VK_MENU);
    return anyBinding([&](KeyBinding const& binding) {
        return binding.action == actionName && binding.vkey == static_cast<UINT>(wParam)
            && modifiersMatch(binding, ctrl, shift, alt);
    });
}

bool KeyMap::matches(EditorKeybind bindingId, WPARAM wParam) {
    bool const ctrl  = currentWindowsModifier(VK_CONTROL);
    bool const shift = currentWindowsModifier(VK_SHIFT);
    bool const alt   = currentWindowsModifier(VK_MENU);
    return anyBinding([&](KeyBinding const& binding) {
        return binding.id == bindingId && binding.vkey == static_cast<UINT>(wParam)
            && modifiersMatch(binding, ctrl, shift, alt);
    });
}

void KeyMap::initialize() {}

bool KeyMap::pressed(EditorKeybind bindingId, bool repeat) {
    if (!ImGui::GetCurrentContext()) return false;
    ImGuiIO const& io = ImGui::GetIO();
    return anyBinding([&](KeyBinding const& binding) {
        if (binding.id != bindingId || !modifiersMatch(binding, io.KeyCtrl, io.KeyShift, io.KeyAlt)) return false;
        ImGuiKey const key = vkeyToImGuiKey(binding.vkey);
        return key != ImGuiKey_None && ImGui::IsKeyPressed(key, repeat);
    });
}

std::string KeyMap::displayString(const std::string& actionName) {
    return bindingDisplayString([&](KeyBinding const& binding) { return binding.action == actionName; });
}

std::string KeyMap::displayString(EditorKeybind bindingId) {
    return bindingDisplayString([&](KeyBinding const& binding) { return binding.id == bindingId; });
}

} // namespace playback::editor::input
