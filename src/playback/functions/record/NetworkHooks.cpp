#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/SubChunkPacket.h"

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

struct NetworkHookState {
    bool levelChunk{};
    bool subChunk{};
    bool completion{};
};

NetworkHookState& networkHookState() {
    static NetworkHookState state;
    return state;
}

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
    auto& replaySession = functions::ReplaySession::getInstance();
    if (replaySession.shouldIsolateChunkPackets()) {
        replaySession.captureNetworkContext(*this);
        if (!packet) {
            origin(source, packet);
            return;
        }
        if (!replaySession.isInjectingPacket(packet.get())) {
            auto const& pos = *packet->mPos;
            if (replaySession.shouldSuppressNativeChunk(pos)) return;

            getLogger().debug("Passing native replay-world LevelChunk for unrecorded column ({}, {})", pos.x, pos.z);
        }

        origin(source, packet);
        return;
    }

    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackSubChunkHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    SubChunkPacket const&    packet
) {
    auto& replaySession = functions::ReplaySession::getInstance();
    if (replaySession.shouldIsolateChunkPackets()) {
        if (replaySession.isInjectingPacket(&packet)) {
            origin(source, packet);
            return;
        }

        auto        filteredPacket = packet;
        auto const& center         = *packet.mCenterPos;
        auto&       entries        = *filteredPacket.mSubChunkData;
        entries.clear();
        entries.reserve(packet.mSubChunkData->size());
        for (auto const& entry : *packet.mSubChunkData) {
            auto const& offset = *entry.mSubChunkPosOffset;
            if (!replaySession.shouldSuppressNativeChunk(
                    ChunkPos{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)}
                )) {
                entries.emplace_back(entry);
            }
        }
        if (entries.empty()) return;

        auto const removed = packet.mSubChunkData->size() - entries.size();
        if (removed != 0) {
            getLogger().debug(
                "Filtered {} recorded entries from a native replay-world SubChunk packet; passing {} unrecorded "
                "entries",
                removed,
                entries.size()
            );
        }

        origin(source, filteredPacket);
        return;
    }

    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackChunkHandleCompletedHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::onChunkHandleCompleted,
    void,
    NetworkIdentifier const& source,
    ChunkPos const&          pos,
    Dimension const&         dimension
) {
    origin(source, pos, dimension);
    functions::ReplaySession::getInstance().onLevelChunkHandled(pos, dimension);
}

bool hookNetwork(bool enable) {
    auto& state = networkHookState();

    auto allInstalled  = [&] { return state.levelChunk && state.subChunk && state.completion; };
    auto noneInstalled = [&] { return !state.levelChunk && !state.subChunk && !state.completion; };
    auto installAll    = [&] {
        if (!state.levelChunk) state.levelChunk = PlaybackLevelChunkHook::hook() == 0;
        if (!state.levelChunk) return false;
        if (!state.subChunk) state.subChunk = PlaybackSubChunkHook::hook() == 0;
        if (!state.subChunk) return false;
        if (!state.completion) state.completion = PlaybackChunkHandleCompletedHook::hook() == 0;
        return state.completion;
    };
    auto removeAll = [&] {
        if (state.completion && PlaybackChunkHandleCompletedHook::unhook()) state.completion = false;
        if (state.subChunk && PlaybackSubChunkHook::unhook()) state.subChunk = false;
        if (state.levelChunk && PlaybackLevelChunkHook::unhook()) state.levelChunk = false;
        return noneInstalled();
    };

    if (enable) {
        if (!hookChunkMutationBarrier(true)) {
            getLogger().error("Unable to install the chunk mutation barrier hooks");
            return false;
        }
        if (allInstalled()) return true;

        if (!installAll()) {
            bool removed = removeAll();
            if (removed) (void)hookChunkMutationBarrier(false);
            getLogger().error(
                "Unable to install replay network hooks (LevelChunk={}, SubChunk={}, completion={}, rollback={})",
                state.levelChunk,
                state.subChunk,
                state.completion,
                removed
            );
            return false;
        }
        return true;
    }

    if (!removeAll()) {
        bool restored = installAll();
        getLogger().error(
            "Unable to remove all replay network hooks; runtime restoration={} (LevelChunk={}, SubChunk={}, "
            "completion={})",
            restored,
            state.levelChunk,
            state.subChunk,
            state.completion
        );
        return false;
    }

    if (!hookChunkMutationBarrier(false)) {
        bool barrierRestored = hookChunkMutationBarrier(true);
        bool networkRestored = barrierRestored && installAll();
        getLogger().error(
            "Unable to remove all chunk mutation barrier hooks; runtime restoration={} (barrier={}, network={})",
            barrierRestored && networkRestored,
            barrierRestored,
            networkRestored
        );
        return false;
    }
    return true;
}

} // namespace playback::functions
