#include "playback/integration/OptiPistonBridge.h"

#include "optipiston/api.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <atomic>

namespace playback::integration {

namespace {

std::atomic<uint64_t> gEpoch{1};
std::atomic<bool>     gClockHeld{};

// Resolved per call so load order between the two mods does not matter.
OptiPistonApiV1 const* findApi() noexcept {
    auto* module = GetModuleHandleW(L"OptiPiston.dll");
    if (!module) return nullptr;
    auto* proc = GetProcAddress(module, OPTIPISTON_API_EXPORT_NAME);
    if (!proc) return nullptr;
    auto const  getApi = reinterpret_cast<OptiPistonGetApiFn>(reinterpret_cast<void*>(proc));
    auto const* api    = getApi(OPTIPISTON_API_VERSION);
    if (!api || api->size < sizeof(OptiPistonApiV1)) return nullptr;
    return api;
}

} // namespace

bool isOptiPistonLoaded() noexcept { return findApi() != nullptr; }

std::optional<OptiPistonSettings> optiPistonSettings() noexcept {
    auto const* api = findApi();
    if (!api) return std::nullopt;
    return OptiPistonSettings{
        api->get_piston_enabled() != 0,
        api->get_piston_duration(),
        api->min_piston_duration,
        api->max_piston_duration,
    };
}

void setOptiPistonEnabled(bool enabled) noexcept {
    if (auto const* api = findApi()) api->set_piston_enabled(enabled ? 1 : 0);
}

void setOptiPistonDuration(float ticks) noexcept {
    if (auto const* api = findApi()) api->set_piston_duration(ticks);
}

void pushReplayPartial(std::optional<float> partial) noexcept {
    if (!gClockHeld.load(std::memory_order_acquire)) return;
    if (auto const* api = findApi()) api->set_external_partial(partial.value_or(-1.0f));
}

void pushReplayClock(int64_t tick, bool jumped) noexcept {
    auto const* api = findApi();
    if (!api) return;
    auto const epoch =
        jumped ? gEpoch.fetch_add(1, std::memory_order_acq_rel) + 1 : gEpoch.load(std::memory_order_acquire);
    api->set_external_clock(tick, epoch);
    gClockHeld.store(true, std::memory_order_release);
}

void clearReplayClock() noexcept {
    if (!gClockHeld.exchange(false, std::memory_order_acq_rel)) return;
    gEpoch.fetch_add(1, std::memory_order_acq_rel);
    if (auto const* api = findApi()) api->clear_external_clock();
}

} // namespace playback::integration
