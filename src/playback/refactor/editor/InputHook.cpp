#include "InputHook.h"

#include "Editor.h"
#include "KeyMap.h"

#include "imgui.h"
#include "imgui_impl_win32.h"

namespace playback::refactor::editor {
namespace InputHook {

bool onWindowsMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 1) ImGui first
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    // 2) Editor-specific shortcuts (grab even if ImGui doesn't want them)
    if (msg == WM_KEYDOWN) {
        if (KeyMap::matches("playback.editor.toggleUI", wParam)) {
            Editor::getInstance().toggle();
            return true;
        }
    }

    // 3) Editor open -> suppress all MCBE input
    if (Editor::getInstance().isOpen()) {
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            return true;
        }
    }

    // 4) Editor closed -> pass through to MCBE
    return false;
}

void syncFrame() {
    // ImGui state is already accumulated during NewFrame via WndProc
    // This function exists as a hook point for future logic
}

bool shouldMCBEConsumeMouse() {
    if (!Editor::getInstance().isOpen()) return true;  // Editor closed: game normal
    return false;                                       // Editor open: game doesn't get mouse
}

bool shouldMCBEConsumeKeyboard() {
    if (!Editor::getInstance().isOpen()) return true;
    return false;
}

} // namespace InputHook
} // namespace playback::refactor::editor