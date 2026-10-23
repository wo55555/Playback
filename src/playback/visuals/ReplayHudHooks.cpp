#include "ReplayHudHooks.h"

#include "ReplayEntityInterpolator.h"
#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/gui/screens/controllers/HudScreenController.h"
#include "mc/client/gui/screens/models/ClientInstanceScreenModel.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/world/level/GameType.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <intrin.h>
#include <optional>

namespace playback::visuals {

namespace {

constexpr int UnknownGameType = -1;

std::atomic<int> gGameType{UnknownGameType};
bool             gInstalled = false;
bool             gCreativeOk{};
bool             gSpectatorOk{};
bool             gSurvivalUiOk{};

// Client RVAs (1.26.20.04) of the two functions that copy the screen model's creative / spectator answers into the
// HUD controller's own flags (HudScreenController::_getDirtyFlag and the initial setup). Every other caller of the
// screen model keeps the observer's real answer.
constexpr uintptr_t DirtyFlagBegin  = 0x2E9D150;
constexpr uintptr_t DirtyFlagEnd    = 0x2E9D304;
constexpr uintptr_t FlagSetupBegin  = 0x2E87140;
constexpr uintptr_t FlagSetupEnd    = 0x2E884A0;

bool isHudFlagCaller(void* returnAddress) {
    static auto const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto const        rva  = reinterpret_cast<uintptr_t>(returnAddress) - base;
    return (rva >= DirtyFlagBegin && rva < DirtyFlagEnd) || (rva >= FlagSetupBegin && rva < FlagSetupEnd);
}

// Only while the view follows the recorded player, and only once its game mode is known.
std::optional<GameType> drivenGameType() {
    if (!isReplaySubjectViewDriven()) return std::nullopt;
    int const value = gGameType.load(std::memory_order_acquire);
    if (value == UnknownGameType) return std::nullopt;
    return static_cast<GameType>(value);
}

LL_TYPE_INSTANCE_HOOK(
    HudScreenModelCreativeHook,
    ll::memory::HookPriority::High,
    MinecraftScreenModel,
    &MinecraftScreenModel::isCreative,
    bool
) {
    bool const value = origin();
    auto const mode  = drivenGameType();
    if (!mode || !isHudFlagCaller(_ReturnAddress())) return value;
    return *mode == GameType::Creative;
}

LL_TYPE_INSTANCE_HOOK(
    HudScreenModelSpectatorHook,
    ll::memory::HookPriority::High,
    MinecraftScreenModel,
    &MinecraftScreenModel::isSpectator,
    bool
) {
    bool const value = origin();
    auto const mode  = drivenGameType();
    if (!mode || !isHudFlagCaller(_ReturnAddress())) return value;
    return *mode == GameType::Spectator;
}

// Vanilla asks the local player directly, so the screen model hooks above cannot reach this answer.
LL_TYPE_INSTANCE_HOOK(
    HudShowSurvivalUiHook,
    ll::memory::HookPriority::High,
    HudScreenController,
    &HudScreenController::_showSurvivalUI,
    bool
) {
    bool const value = origin();
    auto const mode  = drivenGameType();
    if (!mode) return value;
    return *mode != GameType::Creative && *mode != GameType::Spectator;
}

} // namespace

void installReplayHudHooks() {
    if (gInstalled) return;
    gInstalled    = true;
    gCreativeOk   = HudScreenModelCreativeHook::hook() == 0;
    gSpectatorOk  = HudScreenModelSpectatorHook::hook() == 0;
    gSurvivalUiOk = HudShowSurvivalUiHook::hook() == 0;
    auto& logger  = Playback::getInstance().getSelf().getLogger();
    if (gCreativeOk && gSpectatorOk && gSurvivalUiOk) {
        logger.debug("Replay HUD hooks installed");
    } else {
        logger.warn(
            "Replay HUD hooks incomplete: isCreative={} isSpectator={} showSurvivalUi={}",
            gCreativeOk,
            gSpectatorOk,
            gSurvivalUiOk
        );
    }
}

void releaseReplayHudHooks() {
    gGameType.store(UnknownGameType, std::memory_order_release);
    if (!gInstalled) return;
    if (gCreativeOk) HudScreenModelCreativeHook::unhook();
    if (gSpectatorOk) HudScreenModelSpectatorHook::unhook();
    if (gSurvivalUiOk) HudShowSurvivalUiHook::unhook();
    gCreativeOk = gSpectatorOk = gSurvivalUiOk = false;
    gInstalled                                 = false;
}

void setReplayHudGameType(int gameType) { gGameType.store(gameType, std::memory_order_release); }

bool offerReplayHudGameType(int gameType) {
    int expected = UnknownGameType;
    return gGameType.compare_exchange_strong(expected, gameType, std::memory_order_acq_rel);
}

} // namespace playback::visuals
