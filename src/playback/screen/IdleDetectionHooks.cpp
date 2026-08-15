#include "IdleDetectionHooks.h"

#include "playback/exporting/ExportActivity.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/MinecraftGame.h"
#include "mc/client/gui/screens/controllers/MinecraftScreenController.h"
#include "mc/deps/application/AppPlatform.h"
#include "mc/deps/core/platform/AppFocusState.h"

#include <atomic>
#include <functional>
#include <utility>

namespace playback::screen {

namespace {

std::atomic_bool gIdleDetectionGuardInstalled{false};

LL_TYPE_INSTANCE_HOOK(
    PlaybackSuspendWarningModalHook,
    ll::memory::HookPriority::Highest,
    MinecraftScreenController,
    &MinecraftScreenController::_tryShowSuspendWarningModal,
    bool,
    std::function<void()> onConfirm
) {
    if (exporting::isExportActivityActive()) return false;
    return origin(std::move(onConfirm));
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackCanRenderHook,
    ll::memory::HookPriority::Highest,
    MinecraftGame,
    &MinecraftGame::_canRender,
    bool
) {
    if (exporting::isExportActivityActive()) return true;
    return origin();
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

} // namespace

bool hookIdleDetection(bool enable) {
    struct HookState {
        bool warning{};
        bool canRender{};
        bool focusState{};
    };
    static HookState state;

    auto allInstalled  = [&] { return state.warning && state.canRender && state.focusState; };
    auto noneInstalled = [&] { return !state.warning && !state.canRender && !state.focusState; };
    auto installAll    = [&] {
        if (!state.warning) state.warning = PlaybackSuspendWarningModalHook::hook() == 0;
        if (!state.warning) return false;
        if (!state.canRender) state.canRender = PlaybackCanRenderHook::hook() == 0;
        if (!state.canRender) return false;
        if (!state.focusState) state.focusState = PlaybackFocusStateHook::hook() == 0;
        return state.focusState;
    };
    auto removeAll = [&] {
        if (state.focusState && PlaybackFocusStateHook::unhook()) state.focusState = false;
        if (state.canRender && PlaybackCanRenderHook::unhook()) state.canRender = false;
        if (state.warning && PlaybackSuspendWarningModalHook::unhook()) state.warning = false;
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

} // namespace playback::screen
