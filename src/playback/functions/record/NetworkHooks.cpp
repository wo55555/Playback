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
    auto& recorder = functions::Recorder::getInstance();
    if (packet) {
        int dimId = *packet->mDimensionId;
        // getLogger().debug(
        //     "[LevelChunk] pos=({},{}) dimId={} subChunksCount={} cacheEnabled={} tickRange={} "
        //     "clientPacket={} needRequestSubchunks={} requestSubChunkLimit={} "
        //     "serializedChunkSize={} cacheMetadataCount={}",
        //     packet->mPos->x,
        //     packet->mPos->z,
        //     dimId,
        //     packet->mSubChunksCount,
        //     packet->mCacheEnabled,
        //     packet->mIsChunkInTickRange,
        //     packet->isClientPacket,
        //     packet->mClientNeedsToRequestSubchunks,
        //     packet->mClientRequestSubChunkLimit,
        //     packet->mSerializedChunk->size(),
        //     packet->mCacheMetadata->size()
        // );
    }

    if (!recorder.isPaused() && packet) {
        // 将原生网络数据包缓存到 Recorder，供快照时使用
        recorder.cacheChunkPacket(
            packet->mPos,
            packet->mDimensionId,
            packet->mSubChunksCount,
            std::string(packet->mSerializedChunk)
        );
    }
    origin(source, packet);
}

void hookNetwork(bool enable) {
    if (enable) {
        PlaybackLevelChunkHook::hook();
    } else {
        PlaybackLevelChunkHook::unhook();
    }
}

} // namespace playback::functions
