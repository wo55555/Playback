#include "IdleDetectionHooks.h"

#include "playback/editor/exporting/ExportActivity.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/screens/controllers/MinecraftScreenController.h"

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
    if (editor::exporting::isExportActivityActive()) return false;
    return origin(std::move(onConfirm));
}

} // namespace

bool hookIdleDetection(bool enable) {
    if (enable) {
        if (gIdleDetectionGuardInstalled.load(std::memory_order_acquire)) return true;
        if (PlaybackSuspendWarningModalHook::hook() != 0) return false;
        gIdleDetectionGuardInstalled.store(true, std::memory_order_release);
        return true;
    }

    if (!gIdleDetectionGuardInstalled.load(std::memory_order_acquire)) return true;
    if (!PlaybackSuspendWarningModalHook::unhook()) return false;
    gIdleDetectionGuardInstalled.store(false, std::memory_order_release);
    return true;
}

bool isIdleDetectionGuardInstalled() noexcept { return gIdleDetectionGuardInstalled.load(std::memory_order_acquire); }

} // namespace playback::screen
