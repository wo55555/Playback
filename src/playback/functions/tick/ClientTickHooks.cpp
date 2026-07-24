#include "ClientTickHooks.h"

#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/SceneType.h"
#include "mc/client/multiplayer/MultiPlayerLevel.h"

namespace playback::functions {

namespace {

void tickPlayback() {
    switch (playback::Playback::getInstance().getMode()) {
    case playback::PlaybackMode::Record:
        Recorder::getInstance().endTick(false);
        break;
    case playback::PlaybackMode::Replay:
        ReplaySession::getInstance().tick();
        break;
    case playback::PlaybackMode::Unknown:
    default:
        break;
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientUpdateHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$update,
    bool,
    bool isInitFinished
) {
    auto  result     = origin(isInitFinished);
    auto& replay     = ReplaySession::getInstance();
    bool  hudVisible = false;
    if (isInitFinished && replay.isActive()) {
        auto const topScene = static_cast<unsigned int>(getTopSceneType());
        auto const hudScene = static_cast<unsigned int>(ui::SceneType::HudScene);
        hudVisible = (topScene & hudScene) != 0 && isInWorldAndNotShowingAnyMenuScreens() && !isShowingLoadingScreen()
                  && !isShowingProgressScreen();
    }
    editor::tickReplayUI(hudVisible);
    replay.tryFinalizeWorldCleanup();
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientLevelTickHook,
    ll::memory::HookPriority::High,
    MultiPlayerLevel,
    &MultiPlayerLevel::$_subTick,
    void
) {
    ChunkMutationBarrier::setActiveLevel(this);
    origin();
    [[maybe_unused]] auto tickBoundary = ChunkMutationBarrier::enterTickBoundary(*this);
    tickPlayback();
}

bool hookClientTick(bool enable) {
    struct HookState {
        bool update{};
        bool levelTick{};
    };
    static HookState state;

    auto allInstalled  = [&] { return state.update && state.levelTick; };
    auto noneInstalled = [&] { return !state.update && !state.levelTick; };
    auto installAll    = [&] {
        if (!state.update) state.update = PlaybackClientUpdateHook::hook() == 0;
        if (!state.update) return false;
        if (!state.levelTick) state.levelTick = PlaybackClientLevelTickHook::hook() == 0;
        return state.levelTick;
    };
    auto removeAll = [&] {
        if (state.levelTick && PlaybackClientLevelTickHook::unhook()) state.levelTick = false;
        if (state.update && PlaybackClientUpdateHook::unhook()) state.update = false;
        return noneInstalled();
    };

    if (enable) {
        if (allInstalled()) return true;
        if (installAll()) return true;

        bool removed = removeAll();
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install client tick hooks (update={}, levelTick={}, rollback={})",
            state.update,
            state.levelTick,
            removed
        );
        return false;
    }

    if (noneInstalled()) return true;
    if (removeAll()) return true;

    bool restored = installAll();
    Playback::getInstance().getSelf().getLogger().error(
        "Unable to remove client tick hooks (update={}, levelTick={}, restoration={})",
        state.update,
        state.levelTick,
        restored
    );
    return false;
}

} // namespace playback::functions
