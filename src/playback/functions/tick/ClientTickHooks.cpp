#include "ClientTickHooks.h"

#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

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
    PlaybackClientLevelTickHook,
    ll::memory::HookPriority::Normal,
    MultiPlayerLevel,
    &MultiPlayerLevel::$_subTick,
    void
) {
    origin();
    tickPlayback();
}

void hookClientTick(bool enable) {
    if (enable) {
        PlaybackClientLevelTickHook::hook();
    } else {
        PlaybackClientLevelTickHook::unhook();
    }
}

} // namespace playback::functions
