#include "ReplayMouseHook.h"

#include "playback/exporting/ExportActivity.h"
#include "playback/editor/input/EditorInput.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/input/Keyboard.h"
#include "mc/deps/input/MouseAction.h"

#include "imgui.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace playback::editor::host {

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
std::atomic<float> gInputScaleX{1.0f};
std::atomic<float> gInputScaleY{1.0f};
std::atomic<float> gCaptureRequestX{};
std::atomic<float> gCaptureRequestY{};

std::atomic<LONG> gUiCursorScreenX{};
std::atomic<LONG> gUiCursorScreenY{};
std::atomic_bool  gUiCursorScreenValid{};

std::mutex               gQueuedEventsMutex;
std::vector<QueuedEvent> gQueuedEvents;
ll::event::ListenerPtr   gMouseInputListener;
ll::event::ListenerPtr   gKeyInputListener;

std::atomic<uint32_t>   gActiveCallbacks{};
std::mutex              gActiveCallbacksMutex;
std::condition_variable gActiveCallbacksChanged;
thread_local uint32_t   gCallbackDepth{};

std::mutex gOwnershipMutex;

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

bool replayUiOwnsMouse() {
    return gInstalled.load(std::memory_order_acquire) && gReplayUiActive.load(std::memory_order_acquire)
        && gInputActive.load(std::memory_order_acquire);
}

void setMouseOwner(MouseOwner owner) {
    input::setGameInputCaptured(owner == MouseOwner::GameCaptured);
    gMouseOwner.store(owner, std::memory_order_release);
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
    return x >= gGameViewport.left && x < gGameViewport.right && y >= gGameViewport.top && y < gGameViewport.bottom;
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

    gUiCursorScreenX.store(screenPosition.x, std::memory_order_relaxed);
    gUiCursorScreenY.store(screenPosition.y, std::memory_order_relaxed);
    gUiCursorScreenValid.store(true, std::memory_order_release);
    x = static_cast<float>(clientPosition.x) * gInputScaleX.load(std::memory_order_relaxed);
    y = static_cast<float>(clientPosition.y) * gInputScaleY.load(std::memory_order_relaxed);
    return true;
}

void restoreUiCursorPosition() {
    if (!gUiCursorScreenValid.load(std::memory_order_acquire)) return;
    SetCursorPos(gUiCursorScreenX.load(std::memory_order_relaxed), gUiCursorScreenY.load(std::memory_order_relaxed));
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
    bool const hasUiPosition = queryUiCursorPosition(x, y);
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

    if (input::isUiVisible() && input::isGameInputCaptured() && event.keyCode() == Keyboard::Escape) {
        if (event.isDown()) gReleaseRequested.store(true, std::memory_order_release);
        return;
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
    gUiCursorScreenValid.store(false, std::memory_order_release);
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
    if (enable) {
        if (gInstalled.load(std::memory_order_acquire) && gMouseInputListener && gKeyInputListener) return true;

        gInstalled.store(false, std::memory_order_release);
        auto& eventBus      = ll::event::EventBus::getInstance();
        gMouseInputListener = eventBus.emplaceListener<ll::event::MouseInputEvent>(&handleMouseInput);
        gKeyInputListener   = eventBus.emplaceListener<ll::event::KeyInputEvent>(&handleKeyInput);
        if (gMouseInputListener && gKeyInputListener) {
            gInstalled.store(true, std::memory_order_release);
            return true;
        }

        removeListeners();
        input::resetInputState();
        return false;
    }

    gInstalled.store(false, std::memory_order_release);
    gInputActive.store(false, std::memory_order_release);
    setMouseOwner(MouseOwner::Inactive);
    resetOwnershipRequests();
    clearQueuedEvents();
    if (gCallbackDepth != 0) return false;
    removeListeners();
    bool const drained = waitForCallbacks();
    input::resetInputState();
    return drained;
}

void setReplayMouseInputActive(bool active) {
    active = active && gInstalled.load(std::memory_order_acquire) && gReplayUiActive.load(std::memory_order_acquire);
    gInputActive.store(active, std::memory_order_release);
    if (active) return;

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
    gBlockGameMouseInput.store(blockGameMouseInput, std::memory_order_release);
    if (blockGameMouseInput || !inputWasActive) {
        setReplayGameViewport(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (blockGameMouseInput) {
        gCaptureRequested.store(false, std::memory_order_release);
        gReleaseRequested.store(true, std::memory_order_release);
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
                gInputScaleX.store(displayWidth / clientWidth, std::memory_order_relaxed);
                gInputScaleY.store(displayHeight / clientHeight, std::memory_order_relaxed);
            }
        }
    }

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
        return;
    }
    gGameViewport = {left, top, right, bottom};
}

void endReplayMouseFrame() {
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
        gUiCursorScreenValid.store(false, std::memory_order_release);
        return;
    }

    bool const focused = isCurrentProcessForeground();
    if (owner == MouseOwner::Inactive) {
        owner = MouseOwner::UiReleased;
        setMouseOwner(owner);
        gReleaseRequested.store(false, std::memory_order_release);
        if (client.getMouseGrabbed()) {
            client.releaseMouse();
            if (focused) restoreUiCursorPosition();
        }
    } else if (owner == MouseOwner::UiReleased && client.getMouseGrabbed()) {
        client.releaseMouse();
        if (focused) restoreUiCursorPosition();
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
        gReleaseRequested.store(false, std::memory_order_release);
        if (!client.getMouseGrabbed()) client.grabMouse();
        setMouseOwner(MouseOwner::GameCaptured);
        return;
    }

    bool const shouldRelease = owner == MouseOwner::GameCaptured
                            && (gReleaseRequested.exchange(false, std::memory_order_acq_rel)
                                || !gLeftMouseDown.load(std::memory_order_acquire) || !focused
                                || gBlockGameMouseInput.load(std::memory_order_acquire)
                                || gPopupOpen.load(std::memory_order_acquire) || !client.getMouseGrabbed());
    if (!shouldRelease) return;

    if (client.getMouseGrabbed()) client.releaseMouse();
    setMouseOwner(MouseOwner::UiReleased);
    if (focused) restoreUiCursorPosition();
}

} // namespace playback::editor::host
