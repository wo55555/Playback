#include "playback/editor/input/KeyMap.h"

#include <Windows.h>
#include <unordered_map>

namespace playback::editor::input {

namespace {

struct KeyBinding {
    const char* action;
    UINT        vkey;
    bool        ctrl;
    bool        shift;
    bool        alt;
};

// Full key binding table (from §2.12)
const KeyBinding kBindings[] = {
    // File
    {"playback.editor.openReplay",      'O',          true,  false, false},
    {"playback.editor.saveProject",     'S',          true,  false, false},
    {"playback.editor.export",          'E',          true,  false, false},
    // Edit
    {"playback.editor.undo",            'Z',          true,  false, false},
    {"playback.editor.redo",            'Y',          true,  false, false},
    {"playback.editor.delete",          VK_DELETE,    false, false, false},
    {"playback.editor.selectAll",       'A',          true,  false, false},
    {"playback.editor.cancel",          VK_ESCAPE,    false, false, false},
    // Cut/Copy/Paste
    {"playback.editor.cut",             'X',          true,  false, false},
    {"playback.editor.copy",            'C',          true,  false, false},
    {"playback.editor.paste",           'V',          true,  false, false},
    // Playback
    {"playback.editor.playPause",       VK_SPACE,     false, false, false},
    {"playback.editor.jumpStart",       VK_HOME,      false, false, false},
    {"playback.editor.jumpEnd",         VK_END,       false, false, false},
    {"playback.editor.stepLeft",        VK_LEFT,      false, false, false},
    {"playback.editor.stepRight",       VK_RIGHT,     false, false, false},
    {"playback.editor.stepSecondLeft",  VK_LEFT,      false, true,  false},
    {"playback.editor.stepSecondRight", VK_RIGHT,     false, true,  false},
    // Camera
    {"playback.editor.addKeyframe",     'K',          false, false, false},
    {"playback.editor.addCameraTrack",  'N',          true,  true,  false},
    {"playback.editor.camera1",         '1',          false, false, false},
    {"playback.editor.camera2",         '2',          false, false, false},
    {"playback.editor.camera3",         '3',          false, false, false},
    // Clip
    {"playback.editor.splitClip",       'K',          true,  false, false},
    {"playback.editor.setInPoint",      'I',          false, false, false},
    {"playback.editor.setOutPoint",     'O',          false, false, false},
    // Markers
    {"playback.editor.insertMarker",    'M',          false, false, false},
    {"playback.editor.jumpPrevMarker",  VK_OEM_4,     false, false, false}, // [
    {"playback.editor.jumpNextMarker",  VK_OEM_6,     false, false, false}, // ]
    // View
    {"playback.editor.zoomInTimeline",  VK_OEM_PLUS,  false, false, false},
    {"playback.editor.zoomOutTimeline", VK_OEM_MINUS, false, false, false},
    {"playback.editor.resetZoom",       '0',          false, false, false},
    {"playback.editor.toggleUI",        VK_F1,        false, false, false},
    // Editor toggle
    {"playback.editor.toggleEditor",    'E',          true,  false, false},
};

std::unordered_map<std::string, UINT>        gActionToVKey;
std::unordered_map<std::string, std::string> gActionToDisplay;

} // namespace

bool KeyMap::matches(const std::string& actionName, WPARAM wParam) {
    if (gActionToVKey.empty()) initialize();
    auto it = gActionToVKey.find(actionName);
    if (it == gActionToVKey.end()) return false;
    return it->second == static_cast<UINT>(wParam);
}

void KeyMap::initialize() {
    gActionToVKey.clear();
    gActionToDisplay.clear();

    for (const auto& kb : kBindings) {
        gActionToVKey[kb.action] = kb.vkey;

        std::string display;
        if (kb.ctrl) display += "Ctrl+";
        if (kb.shift) display += "Shift+";
        if (kb.alt) display += "Alt+";
        // Map VK codes to display names
        if (kb.vkey == VK_SPACE) display += "Space";
        else if (kb.vkey == VK_DELETE) display += "Del";
        else if (kb.vkey == VK_ESCAPE) display += "Esc";
        else if (kb.vkey == VK_HOME) display += "Home";
        else if (kb.vkey == VK_END) display += "End";
        else if (kb.vkey == VK_LEFT) display += "Left";
        else if (kb.vkey == VK_RIGHT) display += "Right";
        else if (kb.vkey == VK_F1) display += "F1";
        else if (kb.vkey == VK_OEM_4) display += "[";
        else if (kb.vkey == VK_OEM_6) display += "]";
        else if (kb.vkey == VK_OEM_PLUS) display += "+";
        else if (kb.vkey == VK_OEM_MINUS) display += "-";
        else display += static_cast<char>(kb.vkey);

        gActionToDisplay[kb.action] = display;
    }
}

std::string KeyMap::displayString(const std::string& actionName) {
    if (gActionToDisplay.empty()) initialize();
    auto it = gActionToDisplay.find(actionName);
    if (it != gActionToDisplay.end()) return it->second;
    return "";
}

} // namespace playback::editor::input
