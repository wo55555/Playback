#include "EditorInput.h"
#include "KeyMap.h"
#include "playback/exporting/ExportActivity.h"

#include "imgui.h"
#include "mc/deps/input/Keyboard.h"

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
    unsigned character{};
};

constexpr std::size_t MaxQueuedKeyEvents = 256;

std::deque<KeyEvent> gKeyQueue;
std::mutex           gKeyMutex;
std::atomic<bool>    gUiVisible{};
std::atomic<bool>    gGameInputCaptured{};
std::atomic<bool>    gUiKeyboardCaptured{};

std::unordered_set<uint32_t> gImGuiPressedKeys;
std::unordered_set<uint32_t> gPendingImGuiReleases;
std::unordered_set<uint32_t> gGamePressedKeys;
std::unordered_set<uint32_t> gRoutedPressedKeys;
std::unordered_set<uint32_t> gPhysicalPressedKeys;

unsigned textCharacter(uint32_t keyCode, bool shift, bool capsLock) {
    if (keyCode >= Keyboard::Key0 && keyCode <= Keyboard::Key9) {
        if (!shift) return keyCode;
        constexpr char shiftedDigits[] = ")!@#$%^&*(";
        return static_cast<unsigned>(shiftedDigits[keyCode - Keyboard::Key0]);
    }
    if (keyCode >= Keyboard::A && keyCode <= Keyboard::Z) {
        bool const uppercase = shift != capsLock;
        auto const offset    = static_cast<unsigned>(keyCode - Keyboard::A);
        return static_cast<unsigned>(uppercase ? 'A' + offset : 'a' + offset);
    }
    if (keyCode >= Keyboard::Numpad0 && keyCode <= Keyboard::Numpad9) {
        return static_cast<unsigned>('0' + (keyCode - Keyboard::Numpad0));
    }

    switch (keyCode) {
    case Keyboard::Add:
    case Keyboard::Equals:
        return shift || keyCode == Keyboard::Add ? '+' : '=';
    case Keyboard::Subtract:
        return '-';
    case Keyboard::Minus:
        return shift ? '_' : '-';
    case Keyboard::Decimal:
        return '.';
    case Keyboard::Divide:
        return '/';
    case Keyboard::Multiply:
        return '*';
    case Keyboard::Semicolon:
        return shift ? ':' : ';';
    case Keyboard::Comma:
        return shift ? '<' : ',';
    case Keyboard::Period:
        return shift ? '>' : '.';
    case Keyboard::Slash:
        return shift ? '?' : '/';
    case Keyboard::Grave:
        return shift ? '~' : '`';
    case Keyboard::Lbracket:
        return shift ? '{' : '[';
    case Keyboard::Backslash:
        return shift ? '|' : '\\';
    case Keyboard::Rbracket:
        return shift ? '}' : ']';
    case Keyboard::Apostrophe:
        return shift ? '"' : '\'';
    case Keyboard::Space:
        return ' ';
    default:
        return 0;
    }
}

// Map Windows VK code 鈫?ImGuiKey
ImGuiKey vkToImGuiKey(uint32_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<ImGuiKey>(static_cast<int>(ImGuiKey_0) + (vk - '0'));
    }
    switch (vk) {
    case Keyboard::Backspace:
        return ImGuiKey_Backspace;
    case Keyboard::Tab:
        return ImGuiKey_Tab;
    case Keyboard::Return:
        return ImGuiKey_Enter;
    case Keyboard::Up:
        return ImGuiKey_UpArrow;
    case Keyboard::Down:
        return ImGuiKey_DownArrow;
    case Keyboard::PgUp:
        return ImGuiKey_PageUp;
    case Keyboard::PgDown:
        return ImGuiKey_PageDown;
    case Keyboard::Insert:
        return ImGuiKey_Insert;
    case Keyboard::CapsLock:
        return ImGuiKey_CapsLock;
    case Keyboard::Space:
        return ImGuiKey_Space;
    case Keyboard::Home:
        return ImGuiKey_Home;
    case Keyboard::End:
        return ImGuiKey_End;
    case Keyboard::Left:
        return ImGuiKey_LeftArrow;
    case Keyboard::Right:
        return ImGuiKey_RightArrow;
    case Keyboard::Delete:
        return ImGuiKey_Delete;
    case Keyboard::Escape:
        return ImGuiKey_Escape;
    case Keyboard::Lshift:
        return ImGuiKey_LeftShift;
    case Keyboard::Control:
        return ImGuiKey_LeftCtrl;
    case Keyboard::Menu:
        return ImGuiKey_LeftAlt;
    case Keyboard::Equals:
        return ImGuiKey_Equal;
    case Keyboard::Minus:
        return ImGuiKey_Minus;
    case Keyboard::Comma:
        return ImGuiKey_Comma;
    case Keyboard::Period:
        return ImGuiKey_Period;
    case Keyboard::Slash:
        return ImGuiKey_Slash;
    case Keyboard::Semicolon:
        return ImGuiKey_Semicolon;
    case Keyboard::Grave:
        return ImGuiKey_GraveAccent;
    case Keyboard::Lbracket:
        return ImGuiKey_LeftBracket;
    case Keyboard::Backslash:
        return ImGuiKey_Backslash;
    case Keyboard::Rbracket:
        return ImGuiKey_RightBracket;
    case Keyboard::Apostrophe:
        return ImGuiKey_Apostrophe;
    case Keyboard::Numpad0:
    case Keyboard::Numpad1:
    case Keyboard::Numpad2:
    case Keyboard::Numpad3:
    case Keyboard::Numpad4:
    case Keyboard::Numpad5:
    case Keyboard::Numpad6:
    case Keyboard::Numpad7:
    case Keyboard::Numpad8:
    case Keyboard::Numpad9:
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - Keyboard::Numpad0));
    case Keyboard::Decimal:
        return ImGuiKey_KeypadDecimal;
    case Keyboard::Divide:
        return ImGuiKey_KeypadDivide;
    case Keyboard::Multiply:
        return ImGuiKey_KeypadMultiply;
    case Keyboard::Subtract:
        return ImGuiKey_KeypadSubtract;
    case Keyboard::Add:
        return ImGuiKey_KeypadAdd;
    case Keyboard::F1:
        return ImGuiKey_F1;
    default:
        return ImGuiKey_None;
    }
}

void releaseEditorKeysLocked() {
    gKeyQueue.clear();
    gPendingImGuiReleases.insert(gImGuiPressedKeys.begin(), gImGuiPressedKeys.end());
    gImGuiPressedKeys.clear();
    gRoutedPressedKeys.clear();
}

bool isModifier(uint32_t keyCode) {
    return keyCode == Keyboard::Lshift || keyCode == Keyboard::Control || keyCode == Keyboard::Menu;
}

void queueUiEventLocked(uint32_t keyCode, bool down) {
    if (gKeyQueue.size() >= MaxQueuedKeyEvents) releaseEditorKeysLocked();
    bool const     shift = gPhysicalPressedKeys.contains(Keyboard::Lshift);
    bool const     ctrl  = gPhysicalPressedKeys.contains(Keyboard::Control);
    bool const     alt   = gPhysicalPressedKeys.contains(Keyboard::Menu);
    unsigned const character =
        down && !ctrl && !alt ? textCharacter(keyCode, shift, (GetKeyState(VK_CAPITAL) & 1) != 0) : 0;
    gKeyQueue.push_back({keyCode, down, character});
    if (down) gRoutedPressedKeys.insert(keyCode);
    else gRoutedPressedKeys.erase(keyCode);
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
            if (ev.down) {
                gImGuiPressedKeys.insert(ev.keyCode);
                if (ev.character != 0) io.AddInputCharacter(ev.character);
            } else {
                gImGuiPressedKeys.erase(ev.keyCode);
            }
        }
        gKeyQueue.pop_front();
    }
}

bool routeKeyEvent(uint32_t keyCode, bool down, bool forceUi) {
    std::scoped_lock lock(gKeyMutex);

    bool const gameOwned = gGamePressedKeys.contains(keyCode);
    bool const uiOwned   = gRoutedPressedKeys.contains(keyCode);
    if (down) gPhysicalPressedKeys.insert(keyCode);
    else gPhysicalPressedKeys.erase(keyCode);

    if (!down) {
        if (uiOwned) queueUiEventLocked(keyCode, false);
        if (gameOwned) {
            gGamePressedKeys.erase(keyCode);
            return true;
        }
        if (uiOwned) return false;
    }

    if (gameOwned) return true;
    if (uiOwned) {
        queueUiEventLocked(keyCode, true);
        return false;
    }

    bool const uiExclusive = forceUi || gUiKeyboardCaptured.load(std::memory_order_acquire);
    if (uiExclusive) {
        queueUiEventLocked(keyCode, down);
        return false;
    }

    bool const gameOwnsInput =
        !gUiVisible.load(std::memory_order_acquire) || gGameInputCaptured.load(std::memory_order_acquire);
    if (gameOwnsInput) {
        if (down) gGamePressedKeys.insert(keyCode);
        return true;
    }

    if (isModifier(keyCode)) {
        queueUiEventLocked(keyCode, down);
        if (down) gGamePressedKeys.insert(keyCode);
        return true;
    }

    bool const ctrl  = gPhysicalPressedKeys.contains(Keyboard::Control);
    bool const shift = gPhysicalPressedKeys.contains(Keyboard::Lshift);
    bool const alt   = gPhysicalPressedKeys.contains(Keyboard::Menu);
    if (KeyMap::isEditorShortcut(keyCode, ctrl, shift, alt)) {
        queueUiEventLocked(keyCode, down);
        return false;
    }

    if (down) gGamePressedKeys.insert(keyCode);
    return true;
}

void setUiVisible(bool visible) {
    std::scoped_lock lock(gKeyMutex);
    gUiVisible.store(visible, std::memory_order_release);
    if (!visible) {
        gUiKeyboardCaptured.store(false, std::memory_order_release);
        releaseEditorKeysLocked();
    }
}

bool isUiVisible() { return gUiVisible.load(std::memory_order_acquire); }

void setGameInputCaptured(bool captured) {
    std::scoped_lock lock(gKeyMutex);
    bool const       changed = gGameInputCaptured.exchange(captured, std::memory_order_acq_rel) != captured;
    if (captured && changed) releaseEditorKeysLocked();
}

bool isGameInputCaptured() { return gGameInputCaptured.load(std::memory_order_acquire); }

void setUiKeyboardCaptured(bool captured) { gUiKeyboardCaptured.store(captured, std::memory_order_release); }

bool isUiKeyboardCaptured() { return gUiKeyboardCaptured.load(std::memory_order_acquire); }

void releaseKeysForFocusLoss() {
    std::scoped_lock lock(gKeyMutex);
    releaseEditorKeysLocked();
    gGamePressedKeys.clear();
    gPhysicalPressedKeys.clear();
}

void resetInputState() {
    std::scoped_lock lock(gKeyMutex);
    gUiVisible.store(false, std::memory_order_release);
    gGameInputCaptured.store(false, std::memory_order_release);
    gUiKeyboardCaptured.store(false, std::memory_order_release);
    gKeyQueue.clear();
    gImGuiPressedKeys.clear();
    gPendingImGuiReleases.clear();
    gGamePressedKeys.clear();
    gRoutedPressedKeys.clear();
    gPhysicalPressedKeys.clear();
}

bool shouldMCBEConsumeMouse() {
    return !exporting::isExportActivityActive() && (!isUiVisible() || isGameInputCaptured());
}

} // namespace playback::editor::input
