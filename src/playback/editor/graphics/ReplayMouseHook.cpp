#include "ReplayMouseHook.h"

#include "playback/Playback.h"
#include "playback/editor/input/EditorInput.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/exporting/ExportActivity.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseAction.h"

#include "imgui.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace playback::editor::graphics {

namespace {

enum class MouseOwner : uint8_t { Inactive, UiReleased, GameCaptured };
enum class QueuedEventType : uint8_t { Button, Wheel };

struct QueuedEvent {
    QueuedEventType type{};
    float           x{};
    float           y{};
    int             button{};
    bool            down{};
};

struct GameViewportBounds {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

constexpr size_t MaxQueuedEvents      = 512;
constexpr auto   CallbackDrainTimeout = std::chrono::seconds(2);

std::atomic_bool        gInstalled{};
std::atomic_bool        gReplayUiActive{};
std::atomic_bool        gInputActive{};
std::atomic_bool        gBlockGameMouseInput{};
std::atomic_bool        gPopupOpen{};
std::atomic_bool        gFocusKnown{};
std::atomic_bool        gReportedFocused{};
std::atomic_bool        gLeftMouseDown{};
std::atomic_bool        gCaptureRequested{};
std::atomic_bool        gReleaseRequested{};
std::atomic<MouseOwner> gMouseOwner{MouseOwner::Inactive};

std::mutex         gGameViewportMutex;
GameViewportBounds gGameViewport{};
GameViewportBounds gGameViewportExclusion{};
std::atomic<float> gInputScaleX{1.0f};
std::atomic<float> gInputScaleY{1.0f};
std::atomic<float> gCaptureRequestX{};
std::atomic<float> gCaptureRequestY{};

std::mutex               gQueuedEventsMutex;
std::vector<QueuedEvent> gQueuedEvents;
ll::event::ListenerPtr   gMouseInputListener;
ll::event::ListenerPtr   gKeyInputListener;

std::atomic<uint32_t>   gActiveCallbacks{};
std::mutex              gActiveCallbacksMutex;
std::condition_variable gActiveCallbacksChanged;
thread_local uint32_t   gCallbackDepth{};

std::mutex gOwnershipMutex;

// ImGui picks the cursor on the render thread, but SetCursor only takes effect on the window's own thread.
std::atomic<HWND>    gCursorWindow{};
std::atomic<WNDPROC> gCursorOriginalProc{};
std::atomic<int>     gUiCursor{ImGuiMouseCursor_Arrow};
bool                 gCursorOverridden{}; // Window thread only.

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

char const* mouseOwnerName(MouseOwner owner) {
    switch (owner) {
    case MouseOwner::Inactive:
        return "inactive";
    case MouseOwner::UiReleased:
        return "uiReleased";
    case MouseOwner::GameCaptured:
        return "gameCaptured";
    }
    return "unknown";
}

class ActiveCallback {
public:
    ActiveCallback() {
        ++gCallbackDepth;
        gActiveCallbacks.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveCallback() {
        --gCallbackDepth;
        if (gActiveCallbacks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            gActiveCallbacksChanged.notify_all();
        }
    }

    ActiveCallback(ActiveCallback const&)            = delete;
    ActiveCallback& operator=(ActiveCallback const&) = delete;
};

LL_TYPE_INSTANCE_HOOK(
    ReplayMouseGrabHook,
    ll::memory::HookPriority::Highest,
    ClientInstance,
    &ClientInstance::$grabMouse,
    void
) {
    if (input::shouldMCBEConsumeMouse()) origin();
}

bool replayUiOwnsMouse() {
    return gInstalled.load(std::memory_order_acquire) && gReplayUiActive.load(std::memory_order_acquire)
        && gInputActive.load(std::memory_order_acquire);
}

UINT applyCursorMessage() {
    static UINT const message = RegisterWindowMessageW(L"Playback.ApplyUiCursor");
    return message;
}

LPCTSTR systemCursor(int cursor) {
    switch (cursor) {
    case ImGuiMouseCursor_TextInput:
        return IDC_IBEAM;
    case ImGuiMouseCursor_ResizeAll:
        return IDC_SIZEALL;
    case ImGuiMouseCursor_ResizeNS:
        return IDC_SIZENS;
    case ImGuiMouseCursor_ResizeEW:
        return IDC_SIZEWE;
    case ImGuiMouseCursor_ResizeNESW:
        return IDC_SIZENESW;
    case ImGuiMouseCursor_ResizeNWSE:
        return IDC_SIZENWSE;
    case ImGuiMouseCursor_Hand:
        return IDC_HAND;
    case ImGuiMouseCursor_Wait:
        return IDC_WAIT;
    case ImGuiMouseCursor_Progress:
        return IDC_APPSTARTING;
    case ImGuiMouseCursor_NotAllowed:
        return IDC_NO;
    default:
        return nullptr;
    }
}

struct Capsule {
    float ax{};
    float ay{};
    float bx{};
    float by{};
    float radius{};
};

// Hand outlines in a 32-unit box: a rounded palm plus capsule fingers, with crease lines drawn inside.
struct HandShape {
    float                  palmX{};
    float                  palmY{};
    float                  palmHalfWidth{};
    float                  palmHalfHeight{};
    float                  palmRadius{};
    std::array<Capsule, 5> parts{};
    std::array<Capsule, 3> creases{};
    size_t                 creaseCount{};
    float                  hotX{};
    float                  hotY{};
};

// Narrow gaps between fingers fall inside the outline band and read as dark separators.
constexpr HandShape OpenHand{
    17.0f,
    21.0f,
    7.5f,
    6.5f,
    4.5f,
    {{
        {11.8f, 7.0f, 11.8f, 16.0f, 1.9f},
        {16.2f, 5.0f, 16.2f, 16.0f, 1.9f},
        {20.6f, 6.0f, 20.6f, 16.0f, 1.9f},
        {24.8f, 10.0f, 24.2f, 17.5f, 1.7f},
        {10.5f, 23.0f, 5.5f, 16.5f, 2.1f},
    }},
    {},
    0,
    16.0f,
    18.0f,
};

constexpr HandShape ClosedHand{
    17.0f,
    21.5f,
    7.5f,
    6.0f,
    4.5f,
    {{
        {11.8f, 13.2f, 11.8f, 16.0f, 2.1f},
        {16.0f, 12.6f, 16.0f, 16.0f, 2.1f},
        {20.2f, 13.0f, 20.2f, 16.0f, 2.1f},
        {24.2f, 14.4f, 24.2f, 17.0f, 1.9f},
        {10.0f, 22.0f, 7.0f, 18.0f, 2.1f},
    }},
    {{
        {13.9f, 12.0f, 13.9f, 17.5f, 0.0f},
        {18.1f, 11.6f, 18.1f, 17.5f, 0.0f},
        {22.3f, 12.6f, 22.3f, 17.5f, 0.0f},
    }},
    3,
    16.0f,
    19.0f,
};

float capsuleDistance(float x, float y, Capsule const& capsule) {
    float const dx       = capsule.bx - capsule.ax;
    float const dy       = capsule.by - capsule.ay;
    float const lengthSq = dx * dx + dy * dy;
    float const t =
        lengthSq > 0.0f ? std::clamp(((x - capsule.ax) * dx + (y - capsule.ay) * dy) / lengthSq, 0.0f, 1.0f) : 0.0f;
    return std::hypot(x - (capsule.ax + t * dx), y - (capsule.ay + t * dy)) - capsule.radius;
}

float handDistance(float x, float y, HandShape const& hand) {
    float const qx = std::abs(x - hand.palmX) - hand.palmHalfWidth + hand.palmRadius;
    float const qy = std::abs(y - hand.palmY) - hand.palmHalfHeight + hand.palmRadius;
    float       distance =
        std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - hand.palmRadius;
    for (auto const& part : hand.parts) distance = std::min(distance, capsuleDistance(x, y, part));
    return distance;
}

// White fill with a black outline, anti-aliased from the distance field so it stays smooth at any DPI.
HCURSOR createHandCursor(int size, HandShape const& hand) {
    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = size;
    info.bmiHeader.biHeight      = -size;
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void*         bits{};
    HBITMAP const color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;

    // The hand fills most of its 32-unit box; shrinking it about the centre matches the system arrow's height.
    float const scale   = static_cast<float>(size) / 32.0f * 0.72f;
    float const centre  = static_cast<float>(size) * 0.5f;
    float const outline = std::max(1.0f, 1.1f * scale);
    auto* const pixels  = static_cast<uint32_t*>(bits);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            float const x        = (static_cast<float>(px) + 0.5f - centre) / scale + 16.0f;
            float const y        = (static_cast<float>(py) + 0.5f - centre) / scale + 16.0f;
            float const distance = handDistance(x, y, hand) * scale;
            float       crease   = 1.0f;
            for (size_t i = 0; i < hand.creaseCount; ++i) {
                float const d = capsuleDistance(x, y, hand.creases[i]) * scale;
                crease        = std::min(crease, std::clamp(d - outline * 0.5f + 0.5f, 0.0f, 1.0f));
            }
            float const alpha      = std::clamp(outline + 0.5f - distance, 0.0f, 1.0f);
            float const white      = std::clamp(0.5f - distance, 0.0f, 1.0f) * crease;
            auto const  a          = static_cast<uint32_t>(std::lround(alpha * 255.0f));
            auto const  c          = static_cast<uint32_t>(std::lround(white * alpha * 255.0f));
            pixels[py * size + px] = (a << 24) | (c << 16) | (c << 8) | c;
        }
    }

    // The colour bitmap's alpha carries the shape, so the AND mask stays empty.
    std::vector<uint8_t> maskBits(static_cast<size_t>((size + 15) / 16 * 2 * size), 0);
    HBITMAP const        mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    HCURSOR              cursor{};
    if (mask) {
        ICONINFO iconInfo{};
        iconInfo.fIcon    = FALSE;
        iconInfo.xHotspot = static_cast<DWORD>(std::lround((hand.hotX - 16.0f) * scale + centre));
        iconInfo.yHotspot = static_cast<DWORD>(std::lround((hand.hotY - 16.0f) * scale + centre));
        iconInfo.hbmMask  = mask;
        iconInfo.hbmColor = color;
        cursor            = CreateIconIndirect(&iconInfo);
        DeleteObject(mask);
    }
    DeleteObject(color);
    return cursor;
}

// Window thread only. Cursors from an earlier DPI are kept, since one may still be on screen.
HCURSOR handCursor(HWND window, bool closed) {
    static int     cachedSize{};
    static HCURSOR open{};
    static HCURSOR grabbing{};
    int const      size = std::clamp(GetSystemMetricsForDpi(SM_CXCURSOR, GetDpiForWindow(window)), 32, 128);
    if (size != cachedSize) {
        open       = createHandCursor(size, OpenHand);
        grabbing   = createHandCursor(size, ClosedHand);
        cachedSize = size;
    }
    return closed ? grabbing : open;
}

HCURSOR uiCursorHandle(HWND window, int cursor) {
    if (cursor == ui::theme::kCursorGrab) return handCursor(window, false);
    if (cursor == ui::theme::kCursorGrabbing) return handCursor(window, true);
    LPCTSTR const id = systemCursor(cursor);
    return id ? LoadCursor(nullptr, id) : nullptr;
}

// Window thread. Only non-arrow shapes are forced, so the game keeps its own arrow and hidden-cursor handling.
bool applyUiCursor(HWND window) {
    bool const uiCursor = replayUiOwnsMouse() && gMouseOwner.load(std::memory_order_acquire) == MouseOwner::UiReleased;
    HCURSOR const shape = uiCursor ? uiCursorHandle(window, gUiCursor.load(std::memory_order_acquire)) : nullptr;
    if (!shape) {
        if (gCursorOverridden) SetCursor(LoadCursor(nullptr, IDC_ARROW));
        gCursorOverridden = false;
        return false;
    }
    SetCursor(shape);
    gCursorOverridden = true;
    return true;
}

bool isCursorInClient(HWND window) {
    POINT point{};
    RECT  client{};
    return GetCursorPos(&point) && ScreenToClient(window, &point) && GetClientRect(window, &client)
        && PtInRect(&client, point);
}

LRESULT CALLBACK cursorWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && applyUiCursor(window)) return TRUE;
    // WM_SETCURSOR only follows mouse movement, and not at all while the mouse is captured.
    if (message == applyCursorMessage()) {
        if (isCursorInClient(window)) (void)applyUiCursor(window);
        return 0;
    }
    return CallWindowProcW(gCursorOriginalProc.load(std::memory_order_acquire), window, message, wParam, lParam);
}

void installCursorProc(HWND window) {
    // A still-live window may have been subclassed over us, so it keeps the proc rather than moving it.
    HWND const current = gCursorWindow.load(std::memory_order_acquire);
    if (!window || current == window || (current && IsWindow(current))) return;

    auto const original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (!original) return;
    // Published before the swap: the window thread can call the new proc immediately.
    gCursorOriginalProc.store(original, std::memory_order_release);
    SetLastError(0);
    auto const previous = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&cursorWndProc));
    if (!previous && GetLastError() != 0) return;
    gCursorOriginalProc.store(reinterpret_cast<WNDPROC>(previous), std::memory_order_release);
    gCursorWindow.store(window, std::memory_order_release);
}

void removeCursorProc() {
    HWND const window = gCursorWindow.load(std::memory_order_acquire);
    if (!window || !IsWindow(window)) {
        gCursorWindow.store(nullptr, std::memory_order_release);
        return;
    }
    // Another subclass on top still calls us; we stay in its chain as a pass-through.
    if (GetWindowLongPtrW(window, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&cursorWndProc)) return;
    SetWindowLongPtrW(
        window,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(gCursorOriginalProc.load(std::memory_order_acquire))
    );
    gCursorWindow.store(nullptr, std::memory_order_release);
}

void publishUiCursor(int cursor) {
    if (gUiCursor.exchange(cursor, std::memory_order_acq_rel) == cursor) return;
    if (HWND const window = gCursorWindow.load(std::memory_order_acquire)) {
        PostMessageW(window, applyCursorMessage(), 0, 0);
    }
}

void setMouseOwner(MouseOwner owner) {
    auto const previous = gMouseOwner.load(std::memory_order_acquire);
    input::setGameInputCaptured(owner == MouseOwner::GameCaptured);
    gMouseOwner.store(owner, std::memory_order_release);
    if (previous != owner) {
        Playback::getInstance().getSelf().getLogger().debug(
            "Replay mouse owner changed ({} -> {})",
            mouseOwnerName(previous),
            mouseOwnerName(owner)
        );
    }
}

bool isCurrentProcessForeground(HWND* window = nullptr) {
    HWND const foreground = GetForegroundWindow();
    DWORD      processId{};
    if (foreground) GetWindowThreadProcessId(foreground, &processId);
    bool const focused = foreground && processId == GetCurrentProcessId();
    if (window) *window = focused ? foreground : nullptr;
    return focused;
}

bool isGameViewportPoint(float x, float y) {
    std::scoped_lock lock(gGameViewportMutex);
    auto const       inside = [x, y](GameViewportBounds const& b) {
        return x >= b.left && x < b.right && y >= b.top && y < b.bottom;
    };
    return inside(gGameViewport) && !inside(gGameViewportExclusion);
}

int getImGuiMouseButton(char action) {
    switch (action) {
    case MouseAction::ActionLeft:
        return ImGuiMouseButton_Left;
    case MouseAction::ActionRight:
        return ImGuiMouseButton_Right;
    case MouseAction::ActionMiddle:
        return ImGuiMouseButton_Middle;
    case MouseAction::ActionX1:
        return 3;
    case MouseAction::ActionX2:
        return 4;
    default:
        return -1;
    }
}

void queueEvent(QueuedEvent event) {
    if (!replayUiOwnsMouse()) return;

    std::scoped_lock lock(gQueuedEventsMutex);
    if (gQueuedEvents.size() >= MaxQueuedEvents) gQueuedEvents.erase(gQueuedEvents.begin());
    gQueuedEvents.emplace_back(event);
}

bool queryUiCursorPosition(float& x, float& y) {
    HWND window{};
    if (!isCurrentProcessForeground(&window)) return false;

    POINT screenPosition{};
    if (!GetCursorPos(&screenPosition)) return false;
    POINT clientPosition = screenPosition;
    if (!ScreenToClient(window, &clientPosition)) return false;

    x = static_cast<float>(clientPosition.x) * gInputScaleX.load(std::memory_order_relaxed);
    y = static_cast<float>(clientPosition.y) * gInputScaleY.load(std::memory_order_relaxed);
    return true;
}

void handleMouseInput(ll::event::MouseInputEvent& event) {
    ActiveCallback callback;
    bool const     exportActive = exporting::isExportActivityActive();
    if (!replayUiOwnsMouse() && !exportActive) return;

    char const action         = event.actionButtonId();
    bool const blockGameInput = exportActive || gBlockGameMouseInput.load(std::memory_order_acquire);
    bool const popup          = gPopupOpen.load(std::memory_order_acquire);
    auto const owner          = gMouseOwner.load(std::memory_order_acquire);

    if (owner == MouseOwner::GameCaptured) {
        int const button = getImGuiMouseButton(action);
        if (button == ImGuiMouseButton_Left) {
            bool const down = event.buttonData() != MouseAction::DataUp;
            gLeftMouseDown.store(down, std::memory_order_release);
            if (!down) gReleaseRequested.store(true, std::memory_order_release);
        }
        if (blockGameInput) {
            gReleaseRequested.store(true, std::memory_order_release);
            event.cancel();
        }
        return;
    }

    if (action == MouseAction::ActionMove) {
        event.cancel();
        return;
    }

    float      x{};
    float      y{};
    bool const hasUiPosition = !exportActive && queryUiCursorPosition(x, y);
    bool const inGame        = hasUiPosition && !blockGameInput && isGameViewportPoint(x, y);

    int const button = getImGuiMouseButton(action);
    if (button >= 0) {
        bool const down = event.buttonData() != MouseAction::DataUp;
        if (button == ImGuiMouseButton_Left) {
            gLeftMouseDown.store(down, std::memory_order_release);
            if (!down) gCaptureRequested.store(false, std::memory_order_release);
        }

        if (button == ImGuiMouseButton_Left && down && inGame && !popup) {
            gCaptureRequestX.store(x, std::memory_order_relaxed);
            gCaptureRequestY.store(y, std::memory_order_relaxed);
            gCaptureRequested.store(true, std::memory_order_release);
        } else {
            queueEvent({QueuedEventType::Button, 0.0f, 0.0f, button, down});
        }
        event.cancel();
        return;
    }

    if (action == MouseAction::ActionWheel) {
        float wheel = static_cast<float>(event.buttonData());
        if (wheel == 0.0f) wheel = static_cast<float>(event.dy());
        if (wheel != 0.0f) queueEvent({QueuedEventType::Wheel, 0.0f, wheel > 0.0f ? 1.0f : -1.0f});
    }
    event.cancel();
}

void handleKeyInput(ll::event::KeyInputEvent& event) {
    ActiveCallback callback;
    bool const     exportActive = exporting::isExportActivityActive();
    if (!replayUiOwnsMouse() && !exportActive) return;

    bool const topLayerBlocksGame = exportActive || gBlockGameMouseInput.load(std::memory_order_acquire)
                                 || gPopupOpen.load(std::memory_order_acquire)
                                 || (input::isUiKeyboardCaptured() && !input::isGameInputCaptured());
    if ((input::isUiVisible() || exportActive) && topLayerBlocksGame) {
        if (input::isGameInputCaptured() && event.keyCode() == Keyboard::Escape && event.isDown()) {
            gReleaseRequested.store(true, std::memory_order_release);
        }
        if (!input::routeKeyEvent(event.keyCode(), event.isDown(), true)) event.cancel();
        return;
    }

    if (input::isUiVisible() && input::isGameInputCaptured() && event.keyCode() == Keyboard::Escape && event.isDown()) {
        gReleaseRequested.store(true, std::memory_order_release);
    }

    if (!input::routeKeyEvent(event.keyCode(), event.isDown())) event.cancel();
}

void clearQueuedEvents() {
    std::scoped_lock lock(gQueuedEventsMutex);
    gQueuedEvents.clear();
}

void resetOwnershipRequests() {
    gPopupOpen.store(false, std::memory_order_release);
    gBlockGameMouseInput.store(false, std::memory_order_release);
    gCaptureRequested.store(false, std::memory_order_release);
    gReleaseRequested.store(false, std::memory_order_release);
    gLeftMouseDown.store(false, std::memory_order_release);
}

void removeListeners() {
    auto& eventBus = ll::event::EventBus::getInstance();
    if (gMouseInputListener) {
        eventBus.removeListener(gMouseInputListener);
        gMouseInputListener.reset();
    }
    if (gKeyInputListener) {
        eventBus.removeListener(gKeyInputListener);
        gKeyInputListener.reset();
    }
}

bool waitForCallbacks() {
    std::unique_lock lock(gActiveCallbacksMutex);
    return gActiveCallbacksChanged.wait_for(lock, CallbackDrainTimeout, [] {
        return gActiveCallbacks.load(std::memory_order_acquire) == 0;
    });
}

} // namespace

bool hookReplayMouse(bool enable) {
    static bool grabHookInstalled{};
    if (enable) {
        if (gInstalled.load(std::memory_order_acquire) && gMouseInputListener && gKeyInputListener
            && grabHookInstalled) {
            return true;
        }

        gInstalled.store(false, std::memory_order_release);
        if (!grabHookInstalled) grabHookInstalled = ReplayMouseGrabHook::hook() == 0;
        auto& eventBus      = ll::event::EventBus::getInstance();
        gMouseInputListener = eventBus.emplaceListener<ll::event::MouseInputEvent>(&handleMouseInput);
        gKeyInputListener   = eventBus.emplaceListener<ll::event::KeyInputEvent>(&handleKeyInput);
        if (grabHookInstalled && gMouseInputListener && gKeyInputListener) {
            gInstalled.store(true, std::memory_order_release);
            return true;
        }

        removeListeners();
        if (grabHookInstalled && ReplayMouseGrabHook::unhook()) grabHookInstalled = false;
        input::resetInputState();
        return false;
    }

    gInstalled.store(false, std::memory_order_release);
    gInputActive.store(false, std::memory_order_release);
    publishUiCursor(ImGuiMouseCursor_Arrow);
    removeCursorProc();
    setMouseOwner(MouseOwner::Inactive);
    resetOwnershipRequests();
    clearQueuedEvents();
    if (gCallbackDepth != 0) return false;
    removeListeners();
    bool const drained = waitForCallbacks();
    if (grabHookInstalled && ReplayMouseGrabHook::unhook()) grabHookInstalled = false;
    input::resetInputState();
    return drained && !grabHookInstalled;
}

void setReplayMouseInputActive(bool active) {
    active = active && gInstalled.load(std::memory_order_acquire) && gReplayUiActive.load(std::memory_order_acquire);
    gInputActive.store(active, std::memory_order_release);
    if (active) return;

    publishUiCursor(ImGuiMouseCursor_Arrow);
    resetOwnershipRequests();
    gReleaseRequested.store(true, std::memory_order_release);
    input::setGameInputCaptured(false);
    input::setUiKeyboardCaptured(false);
    gFocusKnown.store(false, std::memory_order_release);
    setReplayGameViewport(0.0f, 0.0f, 0.0f, 0.0f);
    clearQueuedEvents();
}

void setReplayUIActive(bool active) {
    gReplayUiActive.store(active, std::memory_order_release);
    if (!active) {
        setReplayMouseInputActive(false);
        input::setUiVisible(false);
    }
}

void beginReplayMouseFrame(float displayWidth, float displayHeight, bool blockGameMouseInput) {
    bool const inputWasActive = gInputActive.load(std::memory_order_acquire);
    bool const exportActive   = exporting::isExportActivityActive();
    gBlockGameMouseInput.store(blockGameMouseInput, std::memory_order_release);
    if (blockGameMouseInput || !inputWasActive) {
        setReplayGameViewport(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (blockGameMouseInput) {
        gCaptureRequested.store(false, std::memory_order_release);
        if (!exportActive) gReleaseRequested.store(true, std::memory_order_release);
        gLeftMouseDown.store(false, std::memory_order_release);
        input::setUiKeyboardCaptured(true);
    }
    setReplayMouseInputActive(true);
    if (!gInputActive.load(std::memory_order_acquire)) return;

    ImGuiIO& io = ImGui::GetIO();

    HWND       foreground{};
    bool const focused         = isCurrentProcessForeground(&foreground);
    bool const focusKnown      = gFocusKnown.load(std::memory_order_acquire);
    bool const reportedFocused = gReportedFocused.load(std::memory_order_acquire);
    if (!focusKnown || focused != reportedFocused) {
        if (!focused) input::releaseKeysForFocusLoss();
        io.AddFocusEvent(focused);
        gReportedFocused.store(focused, std::memory_order_release);
        gFocusKnown.store(true, std::memory_order_release);
    }

    if (focused) {
        RECT clientRect{};
        if (GetClientRect(foreground, &clientRect)) {
            float const clientWidth  = static_cast<float>(clientRect.right - clientRect.left);
            float const clientHeight = static_cast<float>(clientRect.bottom - clientRect.top);
            if (clientWidth > 0.0f && clientHeight > 0.0f) {
                float const scaleX = displayWidth / clientWidth;
                float const scaleY = displayHeight / clientHeight;
                float const oldX   = gInputScaleX.exchange(scaleX, std::memory_order_relaxed);
                float const oldY   = gInputScaleY.exchange(scaleY, std::memory_order_relaxed);
                if (std::abs(oldX - scaleX) > 0.001f || std::abs(oldY - scaleY) > 0.001f) {
                    Playback::getInstance().getSelf().getLogger().debug(
                        "Replay mouse input scale changed (scale={}x{})",
                        scaleX,
                        scaleY
                    );
                }
            }
        }
    }

    // Export still needs the cursor: the progress dialog's cancel button is hit-tested against it.
    if (focused && gMouseOwner.load(std::memory_order_acquire) != MouseOwner::GameCaptured) {
        float x{};
        float y{};
        if (queryUiCursorPosition(x, y)) io.AddMousePosEvent(x, y);
    }

    std::vector<QueuedEvent> events;
    {
        std::scoped_lock lock(gQueuedEventsMutex);
        events.swap(gQueuedEvents);
    }
    for (auto const& event : events) {
        switch (event.type) {
        case QueuedEventType::Button:
            io.AddMouseButtonEvent(event.button, event.down);
            break;
        case QueuedEventType::Wheel:
            io.AddMouseWheelEvent(event.x, event.y);
            break;
        }
    }

    if (gMouseOwner.load(std::memory_order_acquire) == MouseOwner::GameCaptured || !focused) {
        for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) io.AddMouseButtonEvent(button, false);
    }
}

void setReplayGameViewport(float left, float top, float right, float bottom) {
    std::scoped_lock lock(gGameViewportMutex);
    if (right <= left || bottom <= top) {
        gGameViewport = {};
    } else {
        gGameViewport = {left, top, right, bottom};
    }
}

void setReplayGameViewportExclusion(float left, float top, float right, float bottom) {
    std::scoped_lock lock(gGameViewportMutex);
    if (right <= left || bottom <= top) {
        gGameViewportExclusion = {};
    } else {
        gGameViewportExclusion = {left, top, right, bottom};
    }
}

void endReplayMouseFrame(void* window) {
    installCursorProc(static_cast<HWND>(window));
    publishUiCursor(ImGui::GetMouseCursor());
    bool const popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    gPopupOpen.store(popupOpen, std::memory_order_release);
    input::setUiKeyboardCaptured(
        popupOpen || ImGui::GetIO().WantTextInput || (ImGui::IsAnyItemActive() && !input::isGameInputCaptured())
        || gBlockGameMouseInput.load(std::memory_order_acquire)
    );
}

void updateReplayMouseOwnership(ClientInstance& client) {
    std::scoped_lock lock(gOwnershipMutex);

    bool const inputActive = replayUiOwnsMouse();
    auto       owner       = gMouseOwner.load(std::memory_order_acquire);
    if (!inputActive) {
        gCaptureRequested.store(false, std::memory_order_release);
        gReleaseRequested.store(false, std::memory_order_release);
        gLeftMouseDown.store(false, std::memory_order_release);
        setMouseOwner(MouseOwner::Inactive);
        return;
    }

    if (exporting::isExportActivityActive()) {
        gCaptureRequested.store(false, std::memory_order_release);
        gReleaseRequested.store(false, std::memory_order_release);
        gLeftMouseDown.store(false, std::memory_order_release);
        if (client.getMouseGrabbed()) client.releaseMouse();
        setMouseOwner(MouseOwner::UiReleased);
        return;
    }

    bool const focused = isCurrentProcessForeground();
    if (owner == MouseOwner::Inactive) {
        owner = MouseOwner::UiReleased;
        setMouseOwner(owner);
        gReleaseRequested.store(false, std::memory_order_release);
        if (client.getMouseGrabbed()) {
            client.releaseMouse();
        }
    } else if (owner == MouseOwner::UiReleased && client.getMouseGrabbed()) {
        client.releaseMouse();
    }

    if (!focused) {
        gCaptureRequested.store(false, std::memory_order_release);
        gLeftMouseDown.store(false, std::memory_order_release);
    }

    bool const shouldCapture =
        owner == MouseOwner::UiReleased && focused && gLeftMouseDown.load(std::memory_order_acquire)
        && gCaptureRequested.exchange(false, std::memory_order_acq_rel)
        && !gBlockGameMouseInput.load(std::memory_order_acquire) && !gPopupOpen.load(std::memory_order_acquire)
        && isGameViewportPoint(
            gCaptureRequestX.load(std::memory_order_relaxed),
            gCaptureRequestY.load(std::memory_order_relaxed)
        );
    if (shouldCapture) {
        getLogger().debug(
            "Replay mouse capture requested (owner={}, grabbedBefore={})",
            mouseOwnerName(owner),
            client.getMouseGrabbed()
        );
        gReleaseRequested.store(false, std::memory_order_release);
        setMouseOwner(MouseOwner::GameCaptured);
        if (!client.getMouseGrabbed()) client.grabMouse();
        if (!client.getMouseGrabbed()) setMouseOwner(MouseOwner::UiReleased);
        return;
    }

    bool const shouldRelease = owner == MouseOwner::GameCaptured
                            && (gReleaseRequested.exchange(false, std::memory_order_acq_rel)
                                || !gLeftMouseDown.load(std::memory_order_acquire) || !focused
                                || gBlockGameMouseInput.load(std::memory_order_acquire)
                                || gPopupOpen.load(std::memory_order_acquire) || !client.getMouseGrabbed());
    if (!shouldRelease) return;

    getLogger().debug(
        "Replay mouse release requested (owner={}, focused={}, grabbedBefore={})",
        mouseOwnerName(owner),
        focused,
        client.getMouseGrabbed()
    );
    if (client.getMouseGrabbed()) client.releaseMouse();
    setMouseOwner(MouseOwner::UiReleased);
}

} // namespace playback::editor::graphics
