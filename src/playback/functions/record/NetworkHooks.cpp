#include "Recorder.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkStatistics.h"
#include "mc/network/packet/LevelChunkPacket.h"

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PlaybackLevelChunkHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&          source,
    std::shared_ptr<LevelChunkPacket> packet // NOLINT
) {
    if (!playback::Playback::getInstance().isReplayMode() && packet) {
        // 将原生网络数据包缓存到 Recorder，供快照时使用
        functions::Recorder::getInstance().cacheChunkPacket(*packet);
    }
    origin(source, packet);
}

void hookNetwork(bool enable) {
    static bool hooked = false;
    if (hooked == enable) return;

    if (enable) {
        PlaybackLevelChunkHook::hook();
    } else {
        PlaybackLevelChunkHook::unhook();
    }
    hooked = enable;
}

} // namespace playback::functions
