#include "GuiScaleHooks.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/GuiData.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace playback::exporting {

namespace {

std::atomic_bool    gHookInstalled{false};
std::atomic<float>  gForcedGuiScale{0.0f};
std::atomic_int64_t gForcedGuiScaleExpiryMs{0};

// forceGuiScale arms a value that the queued screen size task consumes a frame or so later. Should
// that task never arrive, the override has to lapse rather than pin the scale for the session.
constexpr int64_t ForcedGuiScaleLifetimeMs = 2000;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackForcedGuiScaleHook,
    ll::memory::HookPriority::Lowest,
    GuiData,
    &GuiData::calculateGuiScale,
    float,
    ::Vec2 const&                  totalScreenSize,
    ::Vec2 const&                  safeZone,
    ::cg::math::Rect<float> const& clientViewportModifiers
) {
    // 26.40 returned a non-zero forcedGuiScale verbatim, so its derivation never ran.
    float const forced = gForcedGuiScale.exchange(0.0f);
    if (forced > 0.0f && nowMs() <= gForcedGuiScaleExpiryMs.load(std::memory_order_relaxed)) return forced;
    return origin(totalScreenSize, safeZone, clientViewportModifiers);
}

bool installHook() {
    if (PlaybackForcedGuiScaleHook::hook() != 0) return false;
    gHookInstalled.store(true, std::memory_order_release);
    return true;
}

bool removeHook() {
    if (!PlaybackForcedGuiScaleHook::unhook()) return false;
    clearForcedGuiScale();
    gHookInstalled.store(false, std::memory_order_release);
    return true;
}

} // namespace

bool hookGuiScale(bool enable) {
    if (enable == gHookInstalled.load(std::memory_order_acquire)) return true;
    if (enable) return installHook();
    // A hook that cannot be removed would leave the game calling into unloaded code.
    return removeHook();
}

void forceGuiScale(float scale) {
    if (!(scale > 0.0f)) {
        clearForcedGuiScale();
        return;
    }
    gForcedGuiScaleExpiryMs.store(nowMs() + ForcedGuiScaleLifetimeMs, std::memory_order_relaxed);
    gForcedGuiScale.store(scale, std::memory_order_release);
}

void clearForcedGuiScale() {
    gForcedGuiScale.store(0.0f, std::memory_order_release);
    gForcedGuiScaleExpiryMs.store(0, std::memory_order_relaxed);
}

} // namespace playback::exporting
