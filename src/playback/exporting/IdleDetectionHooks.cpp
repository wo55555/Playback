#include "IdleDetectionHooks.h"

#include "playback/editor/input/EditorInput.h"
#include "playback/exporting/ExportActivity.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/MinecraftGame.h"
#include "mc/client/gui/oreui/Idle.h"
#include "mc/client/gui/oreui/routing/Router.h"
#include "mc/deps/application/AppPlatform.h"
#include "mc/deps/core/platform/AppFocusState.h"

#include <atomic>
#include <string>

namespace playback::exporting {

namespace {

std::atomic_bool gIdleDetectionGuardInstalled{false};

// Playback owns the cursor while its UI is up, so the game sees no input and interrupts itself.
bool shouldSuppressGameInterruptions() {
    if (exporting::isExportActivityActive()) return true;
    return editor::input::isUiVisible() && !editor::input::isGameInputCaptured();
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackFocusStateHook,
    ll::memory::HookPriority::Highest,
    AppPlatform,
    &AppPlatform::$getFocusState,
    AppFocusState
) {
    if (exporting::isExportActivityActive()) return AppFocusState::Focused;
    return origin();
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPauseHook,
    ll::memory::HookPriority::Highest,
    MinecraftGame,
    &MinecraftGame::$openPauseMenu,
    void
) {
    if (shouldSuppressGameInterruptions()) return;
    origin();
}

// The idle screen is an HBUI route, not a native screen, so it has to be blocked at the router.
LL_TYPE_INSTANCE_HOOK(
    PlaybackIdleRouteHook,
    ll::memory::HookPriority::Highest,
    OreUI::Router,
    &OreUI::Router::_pushRoute,
    bool,
    std::string const&            route,
    OreUI::Router::RouterPushMode mode
) {
    if (shouldSuppressGameInterruptions() && route == OreUI::EntryPoints::Idle::ROUTE()) return false;
    return origin(route, mode);
}

} // namespace

bool hookIdleDetection(bool enable) {
    struct HookState {
        bool focusState{};
        bool pause{};
        bool idleRoute{};
    };
    static HookState state;

    auto allInstalled  = [&] { return state.focusState && state.pause && state.idleRoute; };
    auto noneInstalled = [&] { return !state.focusState && !state.pause && !state.idleRoute; };
    auto installAll    = [&] {
        if (!state.focusState) state.focusState = PlaybackFocusStateHook::hook() == 0;
        if (!state.focusState) return false;
        if (!state.pause) state.pause = PlaybackPauseHook::hook() == 0;
        if (!state.pause) return false;
        if (!state.idleRoute) state.idleRoute = PlaybackIdleRouteHook::hook() == 0;
        return state.idleRoute;
    };
    auto removeAll = [&] {
        if (state.idleRoute && PlaybackIdleRouteHook::unhook()) state.idleRoute = false;
        if (state.pause && PlaybackPauseHook::unhook()) state.pause = false;
        if (state.focusState && PlaybackFocusStateHook::unhook()) state.focusState = false;
        return noneInstalled();
    };

    if (enable) {
        if (allInstalled()) {
            gIdleDetectionGuardInstalled.store(true, std::memory_order_release);
            return true;
        }
        if (installAll()) {
            gIdleDetectionGuardInstalled.store(true, std::memory_order_release);
            return true;
        }
        (void)removeAll();
        gIdleDetectionGuardInstalled.store(false, std::memory_order_release);
        return false;
    }

    if (noneInstalled()) {
        gIdleDetectionGuardInstalled.store(false, std::memory_order_release);
        return true;
    }
    if (removeAll()) {
        gIdleDetectionGuardInstalled.store(false, std::memory_order_release);
        return true;
    }

    bool const restored = installAll();
    gIdleDetectionGuardInstalled.store(restored, std::memory_order_release);
    return false;
}

bool isIdleDetectionGuardInstalled() noexcept { return gIdleDetectionGuardInstalled.load(std::memory_order_acquire); }

} // namespace playback::exporting
