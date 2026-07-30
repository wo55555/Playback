#include "InputHook.h"

#include "Editor.h"

#include "imgui.h"

#include <cstdint>
#include <mutex>
#include <queue>

namespace playback::refactor::editor {
namespace InputHook {

namespace {

struct KeyEvent {
    uint32_t keyCode{};
    bool     down{};
};

// Thread-safe key event queue
// Written by MCBE event thread, read by render thread in syncFrame()
std::queue<KeyEvent> gKeyQueue;
std::mutex           gKeyMutex;

// Map Windows VK code → ImGuiKey
ImGuiKey vkToImGuiKey(uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_0) + (vk - '0'));
    }
    switch (vk) {
    case 0x20: return ImGuiKey_Space;
    case 0x24: return ImGuiKey_Home;
    case 0x23: return ImGuiKey_End;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x2E: return ImGuiKey_Delete;
    case 0x1B: return ImGuiKey_Escape;
    case 0x10: return ImGuiKey_LeftShift;
    case 0x11: return ImGuiKey_LeftCtrl;
    case 0x12: return ImGuiKey_LeftAlt;
    case 0xBB: return ImGuiKey_Equal;       // +/= (OEM_PLUS)
    case 0xBD: return ImGuiKey_Minus;       // - (OEM_MINUS)
    case 0xDB: return ImGuiKey_LeftBracket; // [ (OEM_4)
    case 0xDD: return ImGuiKey_RightBracket;// ] (OEM_6)
    case 0x70: return ImGuiKey_F1;          // F1
    default:   return ImGuiKey_None;
    }
}

} // namespace

void syncFrame() {
    // Forward queued key events to ImGui
    ImGuiIO& io = ImGui::GetIO();
    std::scoped_lock lock(gKeyMutex);

    while (!gKeyQueue.empty()) {
        auto const& ev = gKeyQueue.front();
        ImGuiKey key = vkToImGuiKey(ev.keyCode);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, ev.down);
        }
        gKeyQueue.pop();
    }
}

void onKeyEvent(uint32_t keyCode, bool down) {
    std::scoped_lock lock(gKeyMutex);
    gKeyQueue.push({keyCode, down});
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