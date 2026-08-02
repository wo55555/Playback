#include "EditorInput.h"

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace playback::editor::input {

namespace {

struct KeyEvent {
    uint32_t keyCode{};
    bool     down{};
};

constexpr std::size_t MaxQueuedKeyEvents = 256;

std::deque<KeyEvent> gKeyQueue;
std::mutex           gKeyMutex;
std::atomic<bool>    gUiVisible{};
std::atomic<bool>    gGameInputCaptured{};

std::unordered_set<uint32_t> gImGuiPressedKeys;
std::unordered_set<uint32_t> gPendingImGuiReleases;
std::unordered_set<uint32_t> gGamePressedKeys;

// Map Windows VK code → ImGuiKey
ImGuiKey vkToImGuiKey(uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_0) + (vk - '0'));
    }
    switch (vk) {
    case 0x20:
        return ImGuiKey_Space;
    case 0x24:
        return ImGuiKey_Home;
    case 0x23:
        return ImGuiKey_End;
    case 0x25:
        return ImGuiKey_LeftArrow;
    case 0x27:
        return ImGuiKey_RightArrow;
    case 0x2E:
        return ImGuiKey_Delete;
    case 0x1B:
        return ImGuiKey_Escape;
    case 0x10:
        return ImGuiKey_LeftShift;
    case 0x11:
        return ImGuiKey_LeftCtrl;
    case 0x12:
        return ImGuiKey_LeftAlt;
    case 0xBB:
        return ImGuiKey_Equal; // +/= (OEM_PLUS)
    case 0xBD:
        return ImGuiKey_Minus; // - (OEM_MINUS)
    case 0xDB:
        return ImGuiKey_LeftBracket; // [ (OEM_4)
    case 0xDD:
        return ImGuiKey_RightBracket; // ] (OEM_6)
    case 0x70:
        return ImGuiKey_F1; // F1
    default:
        return ImGuiKey_None;
    }
}

void releaseEditorKeysLocked() {
    gKeyQueue.clear();
    gPendingImGuiReleases.insert(gImGuiPressedKeys.begin(), gImGuiPressedKeys.end());
    gImGuiPressedKeys.clear();
}

} // namespace

void syncFrame() {
    ImGuiIO&         io = ImGui::GetIO();
    std::scoped_lock lock(gKeyMutex);

    for (uint32_t keyCode : gPendingImGuiReleases) {
        ImGuiKey const key = vkToImGuiKey(keyCode);
        if (key != ImGuiKey_None) io.AddKeyEvent(key, false);
    }
    gPendingImGuiReleases.clear();

    while (!gKeyQueue.empty()) {
        auto const& ev  = gKeyQueue.front();
        ImGuiKey    key = vkToImGuiKey(ev.keyCode);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, ev.down);
            if (ev.down) gImGuiPressedKeys.insert(ev.keyCode);
            else gImGuiPressedKeys.erase(ev.keyCode);
        }
        gKeyQueue.pop_front();
    }
}

bool routeKeyEvent(uint32_t keyCode, bool down) {
    std::scoped_lock lock(gKeyMutex);

    bool const gameOwnsKey = !gUiVisible.load(std::memory_order_acquire)
                          || gGameInputCaptured.load(std::memory_order_acquire)
                          || gGamePressedKeys.contains(keyCode);
    if (gameOwnsKey) {
        if (down) gGamePressedKeys.insert(keyCode);
        else gGamePressedKeys.erase(keyCode);
        return true;
    }

    if (gKeyQueue.size() >= MaxQueuedKeyEvents) releaseEditorKeysLocked();
    gKeyQueue.push_back({keyCode, down});
    return false;
}

void setUiVisible(bool visible) {
    std::scoped_lock lock(gKeyMutex);
    gUiVisible.store(visible, std::memory_order_release);
    if (!visible) releaseEditorKeysLocked();
}

bool isUiVisible() { return gUiVisible.load(std::memory_order_acquire); }

void setGameInputCaptured(bool captured) {
    std::scoped_lock lock(gKeyMutex);
    bool const      changed = gGameInputCaptured.exchange(captured, std::memory_order_acq_rel) != captured;
    if (captured && changed) releaseEditorKeysLocked();
}

bool isGameInputCaptured() { return gGameInputCaptured.load(std::memory_order_acquire); }

void resetInputState() {
    std::scoped_lock lock(gKeyMutex);
    gUiVisible.store(false, std::memory_order_release);
    gGameInputCaptured.store(false, std::memory_order_release);
    gKeyQueue.clear();
    gImGuiPressedKeys.clear();
    gPendingImGuiReleases.clear();
    gGamePressedKeys.clear();
}

bool shouldMCBEConsumeMouse() { return !isUiVisible() || isGameInputCaptured(); }

} // namespace playback::editor::input
