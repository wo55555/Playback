#include "ReplayHudHooks.h"

#include "ReplayEntityInterpolator.h"
#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"
#include "mc/client/gui/DirtyFlag.h"
#include "mc/client/gui/screens/controllers/HudScreenController.h"
#include "mc/world/level/GameType.h"

#include <atomic>
#include <optional>

namespace playback::visuals {

namespace {

constexpr int UnknownGameType = -1;

std::atomic<int> gGameType{UnknownGameType};
bool             gInstalled = false;
bool             gTickOk{};
bool             gSurvivalUiOk{};

// Only while the view follows the recorded player, and only once its game mode is known.
std::optional<GameType> drivenGameType() {
    // A followed mob has no hotbar or hearts to show; the spectator observer's own HUD stays, as in Flashback.
    if (!isReplaySubjectViewDriven() || !isReplaySubjectPlayer()) return std::nullopt;
    int const value = gGameType.load(std::memory_order_acquire);
    if (value == UnknownGameType) return std::nullopt;
    return static_cast<GameType>(value);
}

// The tick copies the screen model's creative / spectator answers into mIsCreative / mIsSpectator; those flags drive
// the HUD bindings. Overwrite them after the copy. When the replay stops, the next tick sees the observer's answer
// differ from the value left here and rewrites and dirties it itself.
LL_TYPE_INSTANCE_HOOK(
    HudTickHook,
    ll::memory::HookPriority::High,
    HudScreenController,
    &HudScreenController::$tick,
    ui::DirtyFlag
) {
    auto       flags = origin();
    auto const mode  = drivenGameType();
    if (!mode) return flags;
    bool const creative  = *mode == GameType::Creative;
    bool const spectator = *mode == GameType::Spectator;
    if (mIsCreative != creative || mIsSpectator != spectator) {
        mIsCreative  = creative;
        mIsSpectator = spectator;
        flags = static_cast<ui::DirtyFlag>(static_cast<uint>(flags) | static_cast<uint>(ui::DirtyFlag::UpdateBinds));
    }
    return flags;
}

// Vanilla asks the local player directly, so the tick hook above cannot reach this answer.
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
    gTickOk       = HudTickHook::hook() == 0;
    gSurvivalUiOk = HudShowSurvivalUiHook::hook() == 0;
    auto& logger  = Playback::getInstance().getSelf().getLogger();
    if (gTickOk && gSurvivalUiOk) {
        logger.debug("Replay HUD hooks installed");
    } else {
        logger.warn("Replay HUD hooks incomplete: tick={} showSurvivalUi={}", gTickOk, gSurvivalUiOk);
    }
}

void releaseReplayHudHooks() {
    gGameType.store(UnknownGameType, std::memory_order_release);
    if (!gInstalled) return;
    if (gTickOk) HudTickHook::unhook();
    if (gSurvivalUiOk) HudShowSurvivalUiHook::unhook();
    gTickOk = gSurvivalUiOk = false;
    gInstalled              = false;
}

void setReplayHudGameType(int gameType) { gGameType.store(gameType, std::memory_order_release); }

bool offerReplayHudGameType(int gameType) {
    int expected = UnknownGameType;
    return gGameType.compare_exchange_strong(expected, gameType, std::memory_order_acq_rel);
}

} // namespace playback::visuals
