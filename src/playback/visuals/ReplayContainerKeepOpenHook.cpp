#include "ReplayContainerKeepOpenHook.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/gui/screens/controllers/ContainerScreenController.h"

#include <Windows.h>
#include <atomic>

namespace playback::visuals {

namespace {

std::atomic<bool>     gKeepOpen{false};
std::atomic<uint64_t> gOverrides{0};
bool                  gInstalled = false;
bool                  gHooked    = false;

// The replay observer is a spectator, so the validity check of every container screen controller (chest, barrel,
// furnace, dispenser, ...) fails on its first tick and the controller requests to leave. While enabled, a failed
// check is turned into a pass so the previewed screen stays up for the recorded interval.
LL_TYPE_INSTANCE_HOOK(
    ContainerStillValidHook,
    ll::memory::HookPriority::High,
    ContainerScreenController,
    &ContainerScreenController::$_isStillValid,
    bool
) {
    bool const valid = origin();
    if (!valid && gKeepOpen.load(std::memory_order_acquire)) {
        gOverrides.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return valid;
}

void install() {
    auto& logger = Playback::getInstance().getSelf().getLogger();
    gInstalled   = true;
    int const result = ContainerStillValidHook::hook();
    if (result != 0) {
        logger.error("Unable to install the container keep-open hook (code {})", result);
        return;
    }
    gHooked = true;
    auto const target = reinterpret_cast<uintptr_t>(ll::memory::toFuncPtr(&ContainerScreenController::$_isStillValid));
    logger.info(
        "containerKeepOpen hook installed rva=0x{:X}",
        target - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))
    );
}

} // namespace

void setReplayContainerKeepOpen(bool enable) {
    if (enable && !gInstalled) install();
    gKeepOpen.store(enable && gHooked, std::memory_order_release);
}

void releaseReplayContainerKeepOpen() {
    gKeepOpen.store(false, std::memory_order_release);
    if (!gHooked) return;
    if (!ContainerStillValidHook::unhook())
        Playback::getInstance().getSelf().getLogger().error("Unable to remove the container keep-open hook");
    gHooked    = false;
    gInstalled = false;
}

uint64_t replayContainerKeepOpenOverrides() { return gOverrides.load(std::memory_order_relaxed); }

} // namespace playback::visuals
