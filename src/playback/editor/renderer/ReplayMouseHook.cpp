#include "ReplayMouseHook.h"
#include "playback/editor/ui/ReplayUILayout.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseAction.h"

#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace playback::editor::renderer {

namespace {

enum class MouseOwner : uint8_t { Inactive, UiReleased, GameCaptured };
enum class QueuedEventType : uint8_t { Position, Button, Wheel };

constexpr auto MouseHookDrainTimeout = std::chrono::seconds(2);

struct QueuedMouseEvent {
    QueuedEventType type{};
    float           x{};
    float           y{};
    int             button{};
    bool            down{};
};

std::atomic<bool>       gMouseHookActive{false};
std::atomic<bool>       gMouseHookStopping{true};
std::atomic<bool>       gReplayUiInputActive{false};
std::atomic<bool>       gReplayUIActive{false};
std::atomic<bool>       gWantCaptureMouse{false};
std::atomic<bool>       gPopupOpen{false};
std::atomic<bool>       gImGuiReportedWindowFocused{false};
std::atomic<bool>       gImGuiFocusKnown{false};
std::atomic<bool>       gLeftMouseDown{false};
std::atomic<bool>       gCaptureRequested{false};
std::atomic<bool>       gReleaseRequested{false};
std::atomic<MouseOwner> gMouseOwner{MouseOwner::Inactive};

std::atomic<float> gGameViewportLeft{};
std::atomic<float> gGameViewportTop{};
std::atomic<float> gGameViewportRight{1.0f};
std::atomic<float> gGameViewportBottom{1.0f};
std::atomic<float> gInputScaleX{1.0f};
std::atomic<float> gInputScaleY{1.0f};

std::atomic<LONG> gRestoreCursorX{};
std::atomic<LONG> gRestoreCursorY{};
std::atomic<bool> gRestoreCursorValid{false};

std::mutex                    gQueuedEventsMutex;
std::vector<QueuedMouseEvent> gQueuedEvents;
ll::event::ListenerPtr        gMouseInputListener;
ll::event::ListenerPtr        gKeyInputListener;
thread_local bool             gApplyingMouseTransition{};
thread_local uint32_t         gMouseCallbackDepth{};

std::mutex                   gMouseTransitionMutex;
std::condition_variable      gMouseTransitionChanged;
std::atomic<uint32_t>        gActiveMouseCallbacks{};
std::mutex                   gActiveMouseCallbacksMutex;
std::condition_variable      gActiveMouseCallbacksChanged;
std::atomic<DWORD>           gMouseUpdateThreadId{};
std::atomic<ClientInstance*> gLastMouseUpdateClient{};

class ActiveMouseCallback {
public:
    ActiveMouseCallback() {
        ++gMouseCallbackDepth;
        gActiveMouseCallbacks.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveMouseCallback() {
        --gMouseCallbackDepth;
        if (gActiveMouseCallbacks.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            gActiveMouseCallbacksChanged.notify_all();
        }
    }

    ActiveMouseCallback(ActiveMouseCallback const&)            = delete;
    ActiveMouseCallback& operator=(ActiveMouseCallback const&) = delete;
};

bool replayUiOwnsMouse() {
    return gMouseHookActive.load(std::memory_order_acquire) && gReplayUiInputActive.load(std::memory_order_acquire);
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
    return x >= gGameViewportLeft.load(std::memory_order_relaxed)
        && x < gGameViewportRight.load(std::memory_order_relaxed)
        && y >= gGameViewportTop.load(std::memory_order_relaxed)
        && y < gGameViewportBottom.load(std::memory_order_relaxed);
}

void queueEvent(QueuedMouseEvent event) {
    std::scoped_lock lock(gQueuedEventsMutex);
    if (!replayUiOwnsMouse()) return;
    if (event.type == QueuedEventType::Position && !gQueuedEvents.empty()
        && gQueuedEvents.back().type == QueuedEventType::Position) {
        gQueuedEvents.back() = event;
        return;
    }
    gQueuedEvents.emplace_back(event);
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

void saveCursorPosition() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        gRestoreCursorValid.store(false, std::memory_order_release);
        return;
    }
    gRestoreCursorX.store(cursor.x, std::memory_order_relaxed);
    gRestoreCursorY.store(cursor.y, std::memory_order_relaxed);
    gRestoreCursorValid.store(true, std::memory_order_release);
}

void handleMouseInput(ll::event::MouseInputEvent& event) {
    ActiveMouseCallback activeCallback;
    if (!replayUiOwnsMouse()) return;

    char const action = event.actionButtonId();
    // The X and Y coordinates in the event are broken in 26.20.4, so we need to get the cursor position by WINAPI.
    POINT cursor{};
    HWND  window = GetActiveWindow();
    if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor)) {
        return;
    }
    float const x      = static_cast<float>(cursor.x) * gInputScaleX.load(std::memory_order_relaxed);
    float const y      = static_cast<float>(cursor.y) * gInputScaleY.load(std::memory_order_relaxed);
    bool const  inGame = isGameViewportPoint(x, y);
    bool const  popup  = gPopupOpen.load(std::memory_order_acquire);
    auto const  owner  = gMouseOwner.load(std::memory_order_acquire);

    if (owner != MouseOwner::GameCaptured && action == MouseAction::ActionMove) {
        queueEvent({QueuedEventType::Position, x, y});
    }

    int const button = getImGuiMouseButton(action);
    if (button >= 0) {
        bool const down = event.buttonData() != MouseAction::DataUp;
        if (button == ImGuiMouseButton_Left) {
            gLeftMouseDown.store(down, std::memory_order_release);
            if (!down) gCaptureRequested.store(false, std::memory_order_release);
        }

        if (owner == MouseOwner::GameCaptured) {
            if (button == ImGuiMouseButton_Left && !down) {
                gReleaseRequested.store(true, std::memory_order_release);
            }
            return;
        }

        if (button == ImGuiMouseButton_Left && down && inGame && !popup) {
            saveCursorPosition();
            gCaptureRequested.store(true, std::memory_order_release);
            return;
        }

        queueEvent({QueuedEventType::Button, x, y, button, down});
        if (popup || !inGame || gWantCaptureMouse.load(std::memory_order_acquire)) event.cancel();
        return;
    }

    if (action == MouseAction::ActionWheel && owner != MouseOwner::GameCaptured) {
        float wheel = static_cast<float>(event.buttonData());
        if (wheel == 0.0f) wheel = static_cast<float>(event.dy());
        if (wheel != 0.0f) queueEvent({QueuedEventType::Wheel, 0.0f, wheel > 0.0f ? 1.0f : -1.0f});
    }

    if (owner != MouseOwner::GameCaptured && (popup || !inGame || gWantCaptureMouse.load(std::memory_order_acquire))) {
        event.cancel();
    }
}

void handleKeyInput(ll::event::KeyInputEvent& event) {
    ActiveMouseCallback activeCallback;
    if (!gMouseHookActive.load(std::memory_order_acquire) || !functions::ReplaySession::getInstance().isActive()
        || event.keyCode() != Keyboard::Escape) {
        return;
    }

    if (event.isDown() && gMouseOwner.load(std::memory_order_acquire) == MouseOwner::GameCaptured) {
        gReleaseRequested.store(true, std::memory_order_release);
    }
    event.cancel();
}

void applyMouseTransition(ClientInstance& client) {
    std::scoped_lock transitionLock(gMouseTransitionMutex);

    bool const uiActive = replayUiOwnsMouse() && gReplayUIActive.load(std::memory_order_acquire);
    auto       owner    = gMouseOwner.load(std::memory_order_acquire);

    if (!uiActive) {
        gCaptureRequested.store(false, std::memory_order_release);
        gReleaseRequested.store(false, std::memory_order_release);
        gLeftMouseDown.store(false, std::memory_order_release);
        if (owner == MouseOwner::GameCaptured) {
            gApplyingMouseTransition = true;
            if (client.getMouseGrabbed()) client.releaseMouse();
            gApplyingMouseTransition = false;
        }
        gMouseOwner.store(MouseOwner::Inactive, std::memory_order_release);
        gRestoreCursorValid.store(false, std::memory_order_release);
        gMouseTransitionChanged.notify_all();
        return;
    }

    if (owner == MouseOwner::Inactive) {
        owner = MouseOwner::UiReleased;
        gMouseOwner.store(owner, std::memory_order_release);
        gReleaseRequested.store(false, std::memory_order_release);
        if (client.getMouseGrabbed()) {
            gApplyingMouseTransition = true;
            client.releaseMouse();
            gApplyingMouseTransition = false;
        }
    } else if (owner == MouseOwner::UiReleased && client.getMouseGrabbed()) {
        gApplyingMouseTransition = true;
        client.releaseMouse();
        gApplyingMouseTransition = false;
    }

    bool const focused = isCurrentProcessForeground();
    if (!focused) {
        gCaptureRequested.store(false, std::memory_order_release);
        gLeftMouseDown.store(false, std::memory_order_release);
    }
    if (owner == MouseOwner::UiReleased && focused && gLeftMouseDown.load(std::memory_order_acquire)
        && gCaptureRequested.exchange(false, std::memory_order_acq_rel)) {
        gReleaseRequested.store(false, std::memory_order_release);
        gApplyingMouseTransition = true;
        if (!client.getMouseGrabbed()) client.grabMouse();
        gApplyingMouseTransition = false;
        gMouseOwner.store(MouseOwner::GameCaptured, std::memory_order_release);
        return;
    }

    bool const shouldRelease = owner == MouseOwner::GameCaptured
                            && (gReleaseRequested.exchange(false, std::memory_order_acq_rel)
                                || !gLeftMouseDown.load(std::memory_order_acquire) || !focused);
    if (!shouldRelease) return;

    gApplyingMouseTransition = true;
    if (client.getMouseGrabbed()) client.releaseMouse();
    gApplyingMouseTransition = false;
    gMouseOwner.store(MouseOwner::UiReleased, std::memory_order_release);
    gMouseTransitionChanged.notify_all();

    if (focused && gRestoreCursorValid.exchange(false, std::memory_order_acq_rel)) {
        SetCursorPos(gRestoreCursorX.load(std::memory_order_relaxed), gRestoreCursorY.load(std::memory_order_relaxed));
    }
}

LL_TYPE_INSTANCE_HOOK(
    ReplayMouseUpdateHook,
    ll::memory::HookPriority::Low,
    ClientInstance,
    &ClientInstance::$update,
    bool,
    bool isInitFinished
) {
    ActiveMouseCallback activeCallback;
    gMouseUpdateThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    gLastMouseUpdateClient.store(this, std::memory_order_release);
    bool const result = origin(isInitFinished);
    if (gMouseHookActive.load(std::memory_order_acquire)) applyMouseTransition(*this);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    ReplayMouseGrabGuardHook,
    ll::memory::HookPriority::Low,
    ClientInstance,
    &ClientInstance::$grabMouse,
    void
) {
    ActiveMouseCallback activeCallback;
    if (!gApplyingMouseTransition && replayUiOwnsMouse()
        && gMouseOwner.load(std::memory_order_acquire) != MouseOwner::GameCaptured) {
        return;
    }
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    ReplayMouseReleaseGuardHook,
    ll::memory::HookPriority::Low,
    ClientInstance,
    &ClientInstance::$releaseMouse,
    void
) {
    ActiveMouseCallback activeCallback;
    if (!gApplyingMouseTransition && replayUiOwnsMouse()
        && gMouseOwner.load(std::memory_order_acquire) == MouseOwner::GameCaptured && isCurrentProcessForeground()) {
        return;
    }
    origin();
}

struct MouseHookState {
    bool update{};
    bool grab{};
    bool release{};
};

MouseHookState& hookState() {
    static MouseHookState state;
    return state;
}

bool removeHooks(MouseHookState& state) {
    if (state.release && ReplayMouseReleaseGuardHook::unhook()) state.release = false;
    if (state.grab && ReplayMouseGrabGuardHook::unhook()) state.grab = false;
    if (state.update && ReplayMouseUpdateHook::unhook()) state.update = false;
    return !state.update && !state.grab && !state.release;
}

void removeInputListener() {
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

bool waitForActiveMouseCallbacks() {
    std::unique_lock callbacksLock(gActiveMouseCallbacksMutex);
    return gActiveMouseCallbacksChanged.wait_for(callbacksLock, MouseHookDrainTimeout, [] {
        return gActiveMouseCallbacks.load(std::memory_order_acquire) == 0;
    });
}

bool removeMouseHooksAndDrain(MouseHookState& state) {
    removeInputListener();
    if (!removeHooks(state)) return false;
    return waitForActiveMouseCallbacks();
}

} // namespace

bool hookReplayMouse(bool enable) {
    auto& state = hookState();

    if (enable) {
        if (state.update && state.grab && state.release && gMouseInputListener && gKeyInputListener) {
            gMouseHookActive.store(true, std::memory_order_release);
            gMouseHookStopping.store(false, std::memory_order_release);
            return true;
        }

        gMouseHookStopping.store(true, std::memory_order_release);
        setReplayMouseInputActive(false);
        gMouseHookActive.store(false, std::memory_order_release);
        if (gMouseCallbackDepth != 0 || !removeMouseHooksAndDrain(state)) return false;

        state.update = ReplayMouseUpdateHook::hook() == 0;
        if (state.update) state.grab = ReplayMouseGrabGuardHook::hook() == 0;
        if (state.grab) state.release = ReplayMouseReleaseGuardHook::hook() == 0;
        if (state.release) {
            auto& eventBus      = ll::event::EventBus::getInstance();
            gMouseInputListener = eventBus.emplaceListener<ll::event::MouseInputEvent>(&handleMouseInput);
            gKeyInputListener   = eventBus.emplaceListener<ll::event::KeyInputEvent>(&handleKeyInput);
        }
        if (state.update && state.grab && state.release && gMouseInputListener && gKeyInputListener) {
            gMouseHookActive.store(true, std::memory_order_release);
            gMouseHookStopping.store(false, std::memory_order_release);
            return true;
        }

        (void)removeMouseHooksAndDrain(state);
        return false;
    }

    gMouseHookStopping.store(true, std::memory_order_release);
    setReplayMouseInputActive(false);

    if (gMouseCallbackDepth != 0) return false;
    if (gMouseUpdateThreadId.load(std::memory_order_acquire) == GetCurrentThreadId()
        && gMouseOwner.load(std::memory_order_acquire) == MouseOwner::GameCaptured) {
        auto* client = gLastMouseUpdateClient.load(std::memory_order_acquire);
        if (!client) return false;
        applyMouseTransition(*client);
    }

    {
        std::unique_lock transitionLock(gMouseTransitionMutex);
        bool const       released = gMouseTransitionChanged.wait_for(transitionLock, MouseHookDrainTimeout, [] {
            return gMouseOwner.load(std::memory_order_acquire) != MouseOwner::GameCaptured;
        });
        if (!released) return false;
        gMouseOwner.store(MouseOwner::Inactive, std::memory_order_release);
    }

    gMouseHookActive.store(false, std::memory_order_release);
    if (!removeMouseHooksAndDrain(state)) return false;
    gLastMouseUpdateClient.store(nullptr, std::memory_order_release);
    gMouseUpdateThreadId.store(0, std::memory_order_release);
    return true;
}

void setReplayMouseInputActive(bool active) {
    if (active && gMouseHookStopping.load(std::memory_order_acquire)) return;
    gReplayUiInputActive.store(active, std::memory_order_release);
    if (active) {
        if (gMouseHookStopping.load(std::memory_order_acquire)) {
            gReplayUiInputActive.store(false, std::memory_order_release);
        }
        return;
    }

    gWantCaptureMouse.store(false, std::memory_order_release);
    gPopupOpen.store(false, std::memory_order_release);
    gCaptureRequested.store(false, std::memory_order_release);
    gReleaseRequested.store(true, std::memory_order_release);
    gLeftMouseDown.store(false, std::memory_order_release);
    gImGuiFocusKnown.store(false, std::memory_order_release);
    std::scoped_lock lock(gQueuedEventsMutex);
    gQueuedEvents.clear();
}

void setReplayUIActive(bool active) { gReplayUIActive.store(active, std::memory_order_release); }

void beginReplayMouseFrame(ui::ReplayUILayout const& layout, float displayWidth, float displayHeight) {
    gGameViewportLeft.store(layout.gameViewportLeft, std::memory_order_relaxed);
    gGameViewportTop.store(layout.gameViewportTop, std::memory_order_relaxed);
    gGameViewportRight.store(layout.gameViewportRight, std::memory_order_relaxed);
    gGameViewportBottom.store(layout.gameViewportBottom, std::memory_order_relaxed);
    setReplayMouseInputActive(true);
    if (!gReplayUiInputActive.load(std::memory_order_acquire)) return;

    ImGuiIO& io = ImGui::GetIO();

    HWND       foreground{};
    bool const focused         = isCurrentProcessForeground(&foreground);
    bool const focusKnown      = gImGuiFocusKnown.load(std::memory_order_acquire);
    bool const reportedFocused = gImGuiReportedWindowFocused.load(std::memory_order_acquire);
    if (!focusKnown || focused != reportedFocused) {
        io.AddFocusEvent(focused);
        gImGuiReportedWindowFocused.store(focused, std::memory_order_release);
        gImGuiFocusKnown.store(true, std::memory_order_release);
    }

    if (focused) {
        RECT clientRect{};
        if (GetClientRect(foreground, &clientRect)) {
            float const clientWidth  = static_cast<float>(clientRect.right - clientRect.left);
            float const clientHeight = static_cast<float>(clientRect.bottom - clientRect.top);
            if (clientWidth > 0.0f && clientHeight > 0.0f) {
                gInputScaleX.store(displayWidth / clientWidth, std::memory_order_relaxed);
                gInputScaleY.store(displayHeight / clientHeight, std::memory_order_relaxed);
            }
        }
    }

    if (focused && gMouseOwner.load(std::memory_order_acquire) != MouseOwner::GameCaptured) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(foreground, &cursor)) {
            io.AddMousePosEvent(
                static_cast<float>(cursor.x) * gInputScaleX.load(std::memory_order_relaxed),
                static_cast<float>(cursor.y) * gInputScaleY.load(std::memory_order_relaxed)
            );
        }
    }

    std::vector<QueuedMouseEvent> events;
    {
        std::scoped_lock lock(gQueuedEventsMutex);
        events.swap(gQueuedEvents);
    }
    for (auto const& event : events) {
        switch (event.type) {
        case QueuedEventType::Position:
            io.AddMousePosEvent(event.x, event.y);
            break;
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

void endReplayMouseFrame() {
    ImGuiIO const& io = ImGui::GetIO();
    gWantCaptureMouse.store(io.WantCaptureMouse, std::memory_order_release);
    gPopupOpen.store(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId), std::memory_order_release);
}

} // namespace playback::editor::renderer
