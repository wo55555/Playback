#include "ClientTickHooks.h"

#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/world/level/Level.h"

namespace playback::functions {

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientTickHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$tick,
    void
) {
    // auto& playback = playback::Playback::getInstance();
    // if (playback.refreshMode()) {
    //     if (auto level = ll::service::getMultiPlayerLevel()) {
    //         ReplaySession::tryAutoStart(level.value());
    //     }
    // }

    // ReplaySession::getInstance().tick();

    // if (ReplaySession::getInstance().sendRecordedTickPacket()) {
    //     return;
    // }

    Recorder::getInstance().recordTickPacket();

    origin();
}

void hookClientTick(bool enable) {
    if (enable) {
        PlaybackClientTickHook::hook();
    } else {
        PlaybackClientTickHook::unhook();
    }
}

} // namespace playback::functions
