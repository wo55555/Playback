#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkStatistics.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/ActorEventPacket.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/TakeItemActorPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"

#include <variant>

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

struct NetworkHookState {
    bool levelChunk{};
    bool subChunk{};
    bool setTime{};
    bool completion{};
    bool packetObserver{};
    bool packetSender{};
    bool addActor{};
    bool addItemActor{};
    bool removeActor{};
    bool takeItemActor{};
    bool actorEvent{};
    bool levelEvent{};
    bool updateBlock{};
    bool updateBlockSynced{};
    bool updateSubChunkBlocks{};

    [[nodiscard]] bool fastPathHandlersInstalled() const {
        return removeActor && takeItemActor && actorEvent && levelEvent && updateBlock && updateBlockSynced
            && updateSubChunkBlocks;
    }

    [[nodiscard]] bool fastPathHandlersRemoved() const {
        return !removeActor && !takeItemActor && !actorEvent && !levelEvent && !updateBlock && !updateBlockSynced
            && !updateSubChunkBlocks;
    }
};

NetworkHookState& networkHookState() {
    static NetworkHookState state;
    return state;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PlaybackPacketReceivedHook,
    ll::memory::HookPriority::Normal,
    NetworkStatistics,
    &NetworkStatistics::$packetReceivedFrom,
    void,
    NetworkIdentifier const& source,
    Packet const&            packet,
    uint                     size
) {
    auto const& network = this->mNetwork.get();
    if (std::holds_alternative<ClientOrServerNetworkSystemRef::ClientRefT>(network)) {
        Recorder::getInstance().recordGamePacket(packet);
    }
    origin(source, packet, size);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPacketSentHook,
    ll::memory::HookPriority::Normal,
    NetworkStatistics,
    &NetworkStatistics::$packetSentTo,
    void,
    NetworkIdentifier const& target,
    Packet const&            packet,
    uint                     size
) {
    auto const& network = this->mNetwork.get();
    if (std::holds_alternative<ClientOrServerNetworkSystemRef::ServerRefT>(network)) {
        auto  client      = ll::service::getClientInstance();
        auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
        if (localPlayer && target == localPlayer->getNetworkIdentifier()) {
            Recorder::getInstance().recordGamePacket(packet);
        }
    }
    origin(target, packet, size);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackAddActorHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    AddActorPacket const&    packet
) {
    auto const runtimeId = *packet.mRuntimeId;
    origin(source, packet);
    Recorder::getInstance().recordSpawnedActor(runtimeId);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackAddItemActorHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&  source,
    AddItemActorPacket const& packet
) {
    auto const runtimeId = *packet.mRuntimeId;
    origin(source, packet);
    Recorder::getInstance().recordSpawnedActor(runtimeId);
}

#define PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK(HookName, HandlerType, PacketType)                                   \
    LL_TYPE_INSTANCE_HOOK(                                                                                             \
        HookName,                                                                                                      \
        ll::memory::HookPriority::Normal,                                                                              \
        HandlerType,                                                                                                   \
        &HandlerType::$handle,                                                                                         \
        void,                                                                                                          \
        NetworkIdentifier const& source,                                                                               \
        PacketType const&        packet                                                                                \
    ) {                                                                                                                \
        Recorder::getInstance().recordGamePacket(packet);                                                              \
        origin(source, packet);                                                                                        \
    }

#define PLAYBACK_DEFINE_SHARED_CLIENT_HANDLER_HOOK(HookName, HandlerType, PacketType)                                  \
    LL_TYPE_INSTANCE_HOOK(                                                                                             \
        HookName,                                                                                                      \
        ll::memory::HookPriority::Normal,                                                                              \
        HandlerType,                                                                                                   \
        &HandlerType::$handle,                                                                                         \
        void,                                                                                                          \
        NetworkIdentifier const&    source,                                                                            \
        std::shared_ptr<PacketType> packet /* NOLINT */                                                                \
    ) {                                                                                                                \
        if (packet) Recorder::getInstance().recordGamePacket(*packet);                                                 \
        origin(source, packet);                                                                                        \
    }

PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK(PlaybackRemoveActorHook, LegacyClientNetworkHandler, RemoveActorPacket)
PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK(PlaybackTakeItemActorHook, ClientNetworkHandler, TakeItemActorPacket)
PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK(PlaybackActorEventHook, ClientNetworkHandler, ActorEventPacket)
PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK(PlaybackLevelEventHook, ClientNetworkHandler, LevelEventPacket)
PLAYBACK_DEFINE_SHARED_CLIENT_HANDLER_HOOK(PlaybackUpdateBlockHook, LegacyClientNetworkHandler, UpdateBlockPacket)
PLAYBACK_DEFINE_SHARED_CLIENT_HANDLER_HOOK(
    PlaybackUpdateBlockSyncedHook,
    LegacyClientNetworkHandler,
    UpdateBlockSyncedPacket
)
PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK(
    PlaybackUpdateSubChunkBlocksHook,
    ClientNetworkHandler,
    UpdateSubChunkBlocksPacket
)

#undef PLAYBACK_DEFINE_SHARED_CLIENT_HANDLER_HOOK
#undef PLAYBACK_DEFINE_CONST_CLIENT_HANDLER_HOOK

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

    if (packet) Recorder::getInstance().recordGamePacket(*packet);
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

    Recorder::getInstance().recordGamePacket(packet);
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackSetTimeHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    SetTimePacket const&     packet
) {
    auto& replaySession = functions::ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld() && !replaySession.isInjectingPacket(&packet)) return;
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

template <class Hook>
bool installNetworkHook(bool& installed) {
    if (!installed) installed = Hook::hook() == 0;
    return installed;
}

template <class Hook>
void removeNetworkHook(bool& installed) {
    if (installed && Hook::unhook()) installed = false;
}

bool hookNetwork(bool enable) {
    auto& state = networkHookState();
    getLogger().debug(
        "Network hook request (enable={}, LevelChunk={}, SubChunk={}, SetTime={}, completion={}, packetObserver={}, "
        "packetSender={}, spawnHandlers={}, fastPathHandlers={})",
        enable,
        state.levelChunk,
        state.subChunk,
        state.setTime,
        state.completion,
        state.packetObserver,
        state.packetSender,
        state.addActor && state.addItemActor,
        state.fastPathHandlersInstalled()
    );

    auto allInstalled = [&] {
        return state.levelChunk && state.subChunk && state.setTime && state.completion && state.packetObserver
            && state.packetSender && state.addActor && state.addItemActor && state.fastPathHandlersInstalled();
    };
    auto noneInstalled = [&] {
        return !state.levelChunk && !state.subChunk && !state.setTime && !state.completion && !state.packetObserver
            && !state.packetSender && !state.addActor && !state.addItemActor && state.fastPathHandlersRemoved();
    };
    auto installAll = [&] {
        return installNetworkHook<PlaybackLevelChunkHook>(state.levelChunk)
            && installNetworkHook<PlaybackSubChunkHook>(state.subChunk)
            && installNetworkHook<PlaybackSetTimeHook>(state.setTime)
            && installNetworkHook<PlaybackChunkHandleCompletedHook>(state.completion)
            && installNetworkHook<PlaybackPacketReceivedHook>(state.packetObserver)
            && installNetworkHook<PlaybackPacketSentHook>(state.packetSender)
            && installNetworkHook<PlaybackAddActorHook>(state.addActor)
            && installNetworkHook<PlaybackAddItemActorHook>(state.addItemActor)
            && installNetworkHook<PlaybackRemoveActorHook>(state.removeActor)
            && installNetworkHook<PlaybackTakeItemActorHook>(state.takeItemActor)
            && installNetworkHook<PlaybackActorEventHook>(state.actorEvent)
            && installNetworkHook<PlaybackLevelEventHook>(state.levelEvent)
            && installNetworkHook<PlaybackUpdateBlockHook>(state.updateBlock)
            && installNetworkHook<PlaybackUpdateBlockSyncedHook>(state.updateBlockSynced)
            && installNetworkHook<PlaybackUpdateSubChunkBlocksHook>(state.updateSubChunkBlocks);
    };
    auto removeAll = [&] {
        removeNetworkHook<PlaybackUpdateSubChunkBlocksHook>(state.updateSubChunkBlocks);
        removeNetworkHook<PlaybackUpdateBlockSyncedHook>(state.updateBlockSynced);
        removeNetworkHook<PlaybackUpdateBlockHook>(state.updateBlock);
        removeNetworkHook<PlaybackLevelEventHook>(state.levelEvent);
        removeNetworkHook<PlaybackActorEventHook>(state.actorEvent);
        removeNetworkHook<PlaybackTakeItemActorHook>(state.takeItemActor);
        removeNetworkHook<PlaybackRemoveActorHook>(state.removeActor);
        removeNetworkHook<PlaybackAddItemActorHook>(state.addItemActor);
        removeNetworkHook<PlaybackAddActorHook>(state.addActor);
        removeNetworkHook<PlaybackPacketSentHook>(state.packetSender);
        removeNetworkHook<PlaybackPacketReceivedHook>(state.packetObserver);
        removeNetworkHook<PlaybackChunkHandleCompletedHook>(state.completion);
        removeNetworkHook<PlaybackSetTimeHook>(state.setTime);
        removeNetworkHook<PlaybackSubChunkHook>(state.subChunk);
        removeNetworkHook<PlaybackLevelChunkHook>(state.levelChunk);
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
                "Unable to install replay network hooks (LevelChunk={}, SubChunk={}, SetTime={}, completion={}, "
                "packetObserver={}, packetSender={}, spawnHandlers={}, fastPathHandlers={}, rollback={})",
                state.levelChunk,
                state.subChunk,
                state.setTime,
                state.completion,
                state.packetObserver,
                state.packetSender,
                state.addActor && state.addItemActor,
                state.fastPathHandlersInstalled(),
                removed
            );
            return false;
        }
        getLogger().debug("Replay network hooks installed");
        return true;
    }

    if (!removeAll()) {
        bool restored = installAll();
        getLogger().error(
            "Unable to remove all replay network hooks; runtime restoration={} (LevelChunk={}, SubChunk={}, "
            "SetTime={}, completion={}, packetObserver={}, packetSender={}, spawnHandlers={}, fastPathHandlers={})",
            restored,
            state.levelChunk,
            state.subChunk,
            state.setTime,
            state.completion,
            state.packetObserver,
            state.packetSender,
            state.addActor && state.addItemActor,
            state.fastPathHandlersInstalled()
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
    getLogger().debug("Replay network hooks removed");
    return true;
}

} // namespace playback::functions
