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

// No caller range is configured on 1.26.51 for the functions that copy the screen model's creative / spectator
// answers into the HUD controller's own flags, so the screen model keeps the observer's real answer.
bool isHudFlagCaller(void*) { return false; }

// While a replay subject is on screen. Another player's unknown or spectator mode still shows the survival HUD,
// because the observer itself is a spectator and would otherwise hide the hotbar.
std::optional<GameType> drivenGameType() {
    // A followed mob has no hotbar or hearts to show; the spectator observer's own HUD stays.
    if (!isReplaySubjectViewDriven() || !isReplaySubjectPlayer()) return std::nullopt;
    int const value = gGameType.load(std::memory_order_acquire);
    if (!isReplaySubjectRecorder()) {
        if (value == UnknownGameType) return GameType::Survival;
        auto const mode = static_cast<GameType>(value);
        if (mode == GameType::Spectator || mode == GameType::Default || mode == GameType::Undefined) {
            return GameType::Survival;
        }
        return mode;
    }
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
