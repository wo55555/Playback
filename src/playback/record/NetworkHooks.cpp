#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/record/ChunkMutationBarrier.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Memory.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/IPacketHandlerDispatcher.h"
#include "mc/network/NetEventCallback.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/Packet.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/network/packet/InventoryContentPacket.h"
#include "mc/network/packet/InventorySlotPacket.h"
#include "mc/network/packet/ItemData.h"
#include "mc/network/packet/ItemRegistryPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/LevelSoundEventPacket.h"
#include "mc/network/packet/LevelSoundEventPacketPayload.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerHotbarPacket.h"
#include "mc/network/packet/ResourcePackStackPacket.h"
#include "mc/network/packet/ResourcePacksInfoPacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/StartGamePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/platform/brstd/function_ref.h"
#include "mc/world/actor/ActorSoundIdentifier.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/LevelSoundManager.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionArguments.h"



#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace playback::record {
using namespace playback::replay;

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

struct NetworkHookState {
    bool dimensionConstructor{};
    bool levelChunk{};
    bool subChunk{};
    bool setTime{};
    bool resourcePacksInfo{};
    bool resourcePackStack{};
    bool startGame{};
    bool itemRegistry{};
    bool completion{};
    bool packetRead{};
    bool renderFinalizer{};
    bool renderedFov{};
    bool localViewFrame{};
    bool inventorySlot{};
    bool inventoryContent{};
    bool playerHotbar{};
    bool mobEquipment{};
    bool predictedSound{};
    bool localSound{};

    [[nodiscard]] bool hudHandlersInstalled() const {
        return inventorySlot && inventoryContent && playerHotbar && mobEquipment;
    }

    [[nodiscard]] bool hudHandlersRemoved() const {
        return !inventorySlot && !inventoryContent && !playerHotbar && !mobEquipment;
    }

    [[nodiscard]] bool soundHandlersInstalled() const { return predictedSound && localSound; }

    [[nodiscard]] bool soundHandlersRemoved() const { return !predictedSound && !localSound; }

    [[nodiscard]] bool resourceHandlersInstalled() const {
        return resourcePacksInfo && resourcePackStack && startGame && itemRegistry;
    }

    [[nodiscard]] bool resourceHandlersRemoved() const {
        return !resourcePacksInfo && !resourcePackStack && !startGame && !itemRegistry;
    }
};

NetworkHookState& networkHookState() {
    static NetworkHookState state;
    return state;
}

std::atomic<float> gRenderedFov{0.0f};

} // namespace

float latestRenderedFov() noexcept { return gRenderedFov.load(std::memory_order_relaxed); }

LL_TYPE_INSTANCE_HOOK(
    PlaybackResourcePacksInfoHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&       source,
    ResourcePacksInfoPacket const& packet
) {
    auto& replaySession = ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld()) {
        auto recordedInfo = replaySession.getReplayResourcePacksInfo();
        if (recordedInfo) {
            origin(source, *recordedInfo);
            return;
        }
    }
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackDimensionConstructorHook,
    ll::memory::HookPriority::Normal,
    Dimension,
    &Dimension::$ctor,
    void*,
    DimensionArguments&& arguments
) {
    replay::ReplaySession::getInstance().configureReplayDimension(arguments);
    return origin(std::move(arguments));
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackLevelChunkHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&          source,
    std::shared_ptr<LevelChunkPacket> packet // NOLINT
) {
    auto& replaySession = replay::ReplaySession::getInstance();
    if (replaySession.shouldIsolateChunkPackets()) {
        replaySession.captureNetworkContext(*this);
        if (!packet) {
            origin(source, packet);
            return;
        }
        if (!replaySession.isInjectingPacket(packet.get())) {
            auto const& pos             = *packet->mPos;
            auto const  packetDimension = static_cast<DimensionType const&>(packet->mDimensionId);
            if (replaySession.shouldSuppressNativeChunk(pos, packetDimension)) {
                return;
            }
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
    auto& replaySession = replay::ReplaySession::getInstance();
    if (replaySession.shouldIsolateChunkPackets()) {
        if (replaySession.isInjectingPacket(&packet)) {
            origin(source, packet);
            return;
        }

        auto        filteredPacket    = packet;
        auto        suppressedPacket  = packet;
        auto const& center            = *packet.mCenterPos;
        auto const  packetDimension   = static_cast<DimensionType const&>(packet.mDimensionType);
        auto&       filteredEntries   = *filteredPacket.mSubChunkData;
        auto&       suppressedEntries = *suppressedPacket.mSubChunkData;
        filteredEntries.clear();
        filteredEntries.reserve(packet.mSubChunkData->size());
        suppressedEntries.clear();
        suppressedEntries.reserve(packet.mSubChunkData->size());
        for (auto const& entry : *packet.mSubChunkData) {
            auto const& offset = *entry.mSubChunkPosOffset;
            if (replaySession.shouldSuppressNativeChunk(
                    ChunkPos{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)},
                    packetDimension
                )) {
                suppressedEntries.emplace_back(entry);
            } else {
                filteredEntries.emplace_back(entry);
            }
        }

        if (!suppressedEntries.empty()) {
            auto level = ll::service::getMultiPlayerLevel();
            if (level) level->notifySubChunkRequestManager(suppressedPacket);
        }
        if (filteredEntries.empty()) return;

        origin(source, filteredPacket);
        return;
    }

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
    auto& replaySession = replay::ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld() && !replaySession.isInjectingPacket(&packet)) return;
    origin(source, packet);
}

// The replay world's own server keeps resyncing the observer's inventory, which overwrites the HUD the replay shows.
// Only recorded (injected) inventory packets may reach the observer while the replay world is isolated.
bool dropLocalHudPacket(Packet const* packet) {
    auto& replaySession = replay::ReplaySession::getInstance();
    return replaySession.isIsolatingReplayWorld() && !replaySession.isInjectingPacket(packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackInventorySlotHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&   source,
    InventorySlotPacket const& packet
) {
    if (dropLocalHudPacket(&packet)) return;
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackInventoryContentHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&      source,
    InventoryContentPacket const& packet
) {
    if (dropLocalHudPacket(&packet)) return;
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPlayerHotbarHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&  source,
    PlayerHotbarPacket const& packet
) {
    if (dropLocalHudPacket(&packet)) return;
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackMobEquipmentHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&            source,
    std::shared_ptr<MobEquipmentPacket> packet // NOLINT
) {
    if (packet && dropLocalHudPacket(packet.get())) return;
    origin(source, std::move(packet));
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackResourcePackStackHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&       source,
    ResourcePackStackPacket const& packet
) {
    auto& replaySession = ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld()) {
        auto recordedStack = replaySession.getReplayResourcePackStack();
        if (recordedStack) {
            origin(source, *recordedStack);
            return;
        }
    }
    origin(source, packet);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackStartGameHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const& source,
    StartGamePacket const&   packet
) {
    auto& replaySession = ReplaySession::getInstance();
    if (!replaySession.isIsolatingReplayWorld()) {
        origin(source, packet);
        return;
    }

    // The local world keeps its own StartGame; the recorded block properties are digested afterwards.
    origin(source, packet);
    replaySession.applyRecordedBlockRegistry();
}

// The server's own table carries the data-driven item versions and components (icons) that a table rebuilt from the
// client registry loses. The client builds its registry from the first table only, so during replay the local
// world's table is swapped for the recorded one.
LL_TYPE_INSTANCE_HOOK(
    PlaybackItemRegistryHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&  source,
    ItemRegistryPacket const& packet
) {
    auto& replaySession = ReplaySession::getInstance();
    if (!replaySession.isIsolatingReplayWorld() || replaySession.isInjectingPacket(&packet)) {
        origin(source, packet);
        return;
    }

    auto recorded = replaySession.takeRecordedItemRegistry();
    if (!recorded) {
        origin(source, packet);
        return;
    }
    origin(source, *recorded);
    replaySession.logRecordedItemRegistryApplied(*recorded);
}

// Materials are only baked inside this pass, so the recorded blocks must be registered before it runs.
LL_TYPE_INSTANCE_HOOK(
    PlaybackRenderFinalizerHook,
    ll::memory::HookPriority::Normal,
    BlockTypeRegistry,
    &BlockTypeRegistry::finalizeBlockComponentStorageForRendering,
    void,
    brstd::function_ref<void(BlockType&)> finalizer
) {
    auto& replaySession = ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld()) replaySession.applyRecordedBlockRegistry();
    origin(finalizer);
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
    try {
        Recorder::getInstance().recordCompletedChunk(pos, dimension);
    } catch (std::exception const& exception) {
        getLogger().warn("Unable to capture completed chunk ({}, {}): {}", pos.x, pos.z, exception.what());
    } catch (...) {
        getLogger().warn("Unable to capture completed chunk ({}, {})", pos.x, pos.z);
    }
    replay::ReplaySession::getInstance().onLevelChunkHandled(pos, dimension);
}

// The renderer smooths the FOV per frame (option FOV times the sprint/potion modifier); the final value is what replays
// need.
LL_TYPE_INSTANCE_HOOK(
    PlaybackRenderedFovHook,
    ll::memory::HookPriority::Lowest,
    LevelRendererPlayer,
    &LevelRendererPlayer::getFov,
    float,
    float amount,
    bool  enableVariableFov
) {
    float const fov = origin(amount, enableVariableFov);
    if (enableVariableFov) gRenderedFov.store(fov, std::memory_order_relaxed);
    return fov;
}

// The local player turns its view once per frame (input is applied while rendering) but moves once per tick.
LL_TYPE_INSTANCE_HOOK(
    PlaybackLocalViewFrameHook,
    ll::memory::HookPriority::Normal,
    GameRenderer,
    &GameRenderer::renderCurrentFrame,
    void,
    float partialTick
) {
    origin(partialTick);
    Recorder::getInstance().recordLocalViewFrame(partialTick);
}

// The client plays the local player's predicted sounds (hits, landing, strong attacks) itself, and the server sends
// them to every other player only, so they never arrive as packets.
LL_TYPE_INSTANCE_HOOK(
    PlaybackPredictedSoundHook,
    ll::memory::HookPriority::Normal,
    LevelSoundManager,
    &LevelSoundManager::playPredictiveSynchronizedSound,
    void,
    IDimension&                          dimension,
    SharedTypes::Legacy::LevelSoundEvent type,
    Vec3 const&                          pos,
    Player*                              targetPlayer,
    int                                  data,
    ActorSoundIdentifier const&          actor,
    bool                                 isGlobal,
    std::optional<Vec3> const&           fireAtPosition
) {
    origin(dimension, type, pos, targetPlayer, data, actor, isGlobal, fireAtPosition);
    auto& recorder = Recorder::getInstance();
    if (!mIsClientSide || !recorder.isActive() || recorder.isPaused()) return;
    recorder.recordGamePacket(
        LevelSoundEventPacket(LevelSoundEventPacketPayload(type, pos, data, actor, isGlobal, fireAtPosition))
    );
}

using DimensionSoundFn = void (Level::*)(
    DimensionType,
    SharedTypes::Legacy::LevelSoundEvent,
    Vec3 const&,
    int,
    ActorSoundIdentifier const&,
    bool
);

// The client plays each actor's own sounds (steps, jumps) itself, so the local player's never arrive as packets.
LL_TYPE_INSTANCE_HOOK(
    PlaybackLocalSoundHook,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<DimensionSoundFn>(&Level::$playSound),
    void,
    DimensionType                        dimension,
    SharedTypes::Legacy::LevelSoundEvent type,
    Vec3 const&                          pos,
    int                                  data,
    ActorSoundIdentifier const&          actor,
    bool                                 isGlobal
) {
    if (!mIsClientSide) {
        origin(dimension, type, pos, data, actor, isGlobal);
        return;
    }
    auto const actorId       = actor.mActorId->rawID;
    auto&      replaySession = ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld()) {
        // The recording already carries the recorded player's own sounds, so the client's copies would repeat them.
        if (actorId == RecordedPlayerUniqueIdValue) return;
        origin(dimension, type, pos, data, actor, isGlobal);
        return;
    }
    origin(dimension, type, pos, data, actor, isGlobal);
    auto& recorder = Recorder::getInstance();
    if (!recorder.isActive() || recorder.isPaused()) return;
    auto  client      = ll::service::getClientInstance();
    auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
    if (!localPlayer || actorId != localPlayer->getOrCreateUniqueID().rawID) return;
    recorder.recordGamePacket(
        LevelSoundEventPacket(LevelSoundEventPacketPayload(type, pos, data, actor, isGlobal, std::nullopt))
    );
}

namespace {

// Every packet the client handles passes through its type's dispatcher, so recording there keeps each one once, on the
// tick the client applies it, whichever connection delivered it.
using PacketDispatchFn =
    void (*)(IPacketHandlerDispatcher const*, NetworkIdentifier const&, NetEventCallback&, std::shared_ptr<Packet>&);

constexpr size_t PacketDispatchSlot = 1;

// Entries are never erased, so a dispatch already past its lookup can still call its original.
struct PacketDispatchers {
    std::shared_mutex                            mutex;
    std::unordered_map<void**, PacketDispatchFn> originals;
};

PacketDispatchers& packetDispatchers() {
    static PacketDispatchers dispatchers;
    return dispatchers;
}

void** dispatcherVtable(IPacketHandlerDispatcher const* dispatcher) {
    return *reinterpret_cast<void** const*>(dispatcher);
}

void setDispatchSlot(void** vtable, void* function) {
    ll::memory::modify(&vtable[PacketDispatchSlot], sizeof(void*), [&] { vtable[PacketDispatchSlot] = function; });
}

struct ReadPacket {
    Packet const* packet{};
    std::string   bytes;
};

// A decoded packet cannot always be written back out, so the bytes it was read from are kept until its dispatch,
// which follows on the same thread.
thread_local ReadPacket gReadPacket;

bool isIntegratedServer(NetEventCallback const& callback) {
    auto server = ll::service::getServerNetworkHandler();
    return server && &callback == static_cast<NetEventCallback const*>(server.as_ptr());
}

void dispatchPacket(
    IPacketHandlerDispatcher const* dispatcher,
    NetworkIdentifier const&        source,
    NetEventCallback&               callback,
    std::shared_ptr<Packet>&        packet
) {
    PacketDispatchFn original;
    {
        auto&            dispatchers = packetDispatchers();
        std::shared_lock lock(dispatchers.mutex);
        original = dispatchers.originals.at(dispatcherVtable(dispatcher));
    }
    std::string payload;
    if (packet && gReadPacket.packet == packet.get()) payload = std::move(gReadPacket.bytes);
    gReadPacket.packet = nullptr;
    if (!packet || isIntegratedServer(callback) || ReplaySession::getInstance().isIsolatingReplayWorld()) {
        original(dispatcher, source, callback, packet);
        return;
    }
    auto const received = packet;
    auto&      recorder = Recorder::getInstance();
    recorder.recordReceivedPacket(*received, payload);
    original(dispatcher, source, callback, packet);
    recorder.recordHandledPacket(*received);
}

// Mods load before the game can create packets, so each dispatcher is wrapped when its first packet is read, which
// precedes that packet's dispatch on the same thread.
void wrapDispatcher(IPacketHandlerDispatcher const* dispatcher) {
    if (!dispatcher) return;
    auto** const vtable = dispatcherVtable(dispatcher);
    auto* const  detour = reinterpret_cast<void*>(&dispatchPacket);
    if (std::atomic_ref(vtable[PacketDispatchSlot]).load(std::memory_order_acquire) == detour) return;
    auto&            dispatchers = packetDispatchers();
    std::unique_lock lock(dispatchers.mutex);
    if (vtable[PacketDispatchSlot] == detour) return;
    dispatchers.originals[vtable] = reinterpret_cast<PacketDispatchFn>(vtable[PacketDispatchSlot]);
    setDispatchSlot(vtable, detour);
}

void unwrapDispatchers() {
    auto&            dispatchers = packetDispatchers();
    std::unique_lock lock(dispatchers.mutex);
    // A slot another mod has since taken keeps its chain.
    for (auto const& [vtable, original] : dispatchers.originals) {
        if (vtable[PacketDispatchSlot] == reinterpret_cast<void*>(&dispatchPacket)) {
            setDispatchSlot(vtable, reinterpret_cast<void*>(original));
        }
    }
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackPacketReadHook,
    ll::memory::HookPriority::Normal,
    Packet,
    &Packet::readNoHeader,
    Bedrock::Result<void>,
    ReadOnlyBinaryStream&        stream,
    cereal::ReflectionCtx const& reflectionCtx,
    SubClientId const&           subId
) {
    wrapDispatcher(mHandler);
    auto const begin  = stream.mReadPointer;
    auto       result = origin(stream, reflectionCtx, subId);
    if (result) {
        gReadPacket.packet = this;
        gReadPacket.bytes.assign(stream.mView.data() + begin, stream.mReadPointer - begin);
    }
    return result;
}

} // namespace

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

    auto allInstalled = [&] {
        return state.dimensionConstructor && state.levelChunk && state.subChunk && state.setTime
            && state.resourceHandlersInstalled() && state.completion && state.packetRead && state.renderFinalizer
            && state.renderedFov && state.localViewFrame && state.hudHandlersInstalled()
            && state.soundHandlersInstalled();
    };
    auto noneInstalled = [&] {
        return !state.dimensionConstructor && !state.levelChunk && !state.subChunk && !state.setTime
            && state.resourceHandlersRemoved() && !state.completion && !state.packetRead && !state.renderFinalizer
            && !state.renderedFov && !state.localViewFrame && state.hudHandlersRemoved()
            && state.soundHandlersRemoved();
    };
    auto installAll = [&] {
        return installNetworkHook<PlaybackDimensionConstructorHook>(state.dimensionConstructor)
            && installNetworkHook<PlaybackLevelChunkHook>(state.levelChunk)
            && installNetworkHook<PlaybackSubChunkHook>(state.subChunk)
            && installNetworkHook<PlaybackSetTimeHook>(state.setTime)
            && installNetworkHook<PlaybackResourcePacksInfoHook>(state.resourcePacksInfo)
            && installNetworkHook<PlaybackResourcePackStackHook>(state.resourcePackStack)
            && installNetworkHook<PlaybackStartGameHook>(state.startGame)
            && installNetworkHook<PlaybackItemRegistryHook>(state.itemRegistry)
            && installNetworkHook<PlaybackChunkHandleCompletedHook>(state.completion)
            && installNetworkHook<PlaybackPacketReadHook>(state.packetRead)
            && installNetworkHook<PlaybackRenderFinalizerHook>(state.renderFinalizer)
            && installNetworkHook<PlaybackRenderedFovHook>(state.renderedFov)
            && installNetworkHook<PlaybackLocalViewFrameHook>(state.localViewFrame)
            && installNetworkHook<PlaybackInventorySlotHook>(state.inventorySlot)
            && installNetworkHook<PlaybackInventoryContentHook>(state.inventoryContent)
            && installNetworkHook<PlaybackPlayerHotbarHook>(state.playerHotbar)
            && installNetworkHook<PlaybackMobEquipmentHook>(state.mobEquipment)
            && installNetworkHook<PlaybackPredictedSoundHook>(state.predictedSound)
            && installNetworkHook<PlaybackLocalSoundHook>(state.localSound);
    };
    auto removeAll = [&] {
        removeNetworkHook<PlaybackLocalSoundHook>(state.localSound);
        removeNetworkHook<PlaybackPredictedSoundHook>(state.predictedSound);
        removeNetworkHook<PlaybackMobEquipmentHook>(state.mobEquipment);
        removeNetworkHook<PlaybackPlayerHotbarHook>(state.playerHotbar);
        removeNetworkHook<PlaybackInventoryContentHook>(state.inventoryContent);
        removeNetworkHook<PlaybackInventorySlotHook>(state.inventorySlot);
        removeNetworkHook<PlaybackLocalViewFrameHook>(state.localViewFrame);
        removeNetworkHook<PlaybackRenderedFovHook>(state.renderedFov);
        removeNetworkHook<PlaybackRenderFinalizerHook>(state.renderFinalizer);
        removeNetworkHook<PlaybackPacketReadHook>(state.packetRead);
        if (!state.packetRead) unwrapDispatchers();
        removeNetworkHook<PlaybackChunkHandleCompletedHook>(state.completion);
        removeNetworkHook<PlaybackItemRegistryHook>(state.itemRegistry);
        removeNetworkHook<PlaybackStartGameHook>(state.startGame);
        removeNetworkHook<PlaybackResourcePackStackHook>(state.resourcePackStack);
        removeNetworkHook<PlaybackResourcePacksInfoHook>(state.resourcePacksInfo);
        removeNetworkHook<PlaybackSetTimeHook>(state.setTime);
        removeNetworkHook<PlaybackSubChunkHook>(state.subChunk);
        removeNetworkHook<PlaybackLevelChunkHook>(state.levelChunk);
        removeNetworkHook<PlaybackDimensionConstructorHook>(state.dimensionConstructor);
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
                "Unable to install replay network hooks (dimensionConstructor={}, LevelChunk={}, SubChunk={}, "
                "SetTime={}, resourceHandlers={}, completion={}, packetRead={}, soundHandlers={}, "
                "rollback={})",
                state.dimensionConstructor,
                state.levelChunk,
                state.subChunk,
                state.setTime,
                state.resourceHandlersInstalled(),
                state.completion,
                state.packetRead,
                state.soundHandlersInstalled(),
                removed
            );
            return false;
        }
        return true;
    }

    if (!removeAll()) {
        bool restored = installAll();
        getLogger().error(
            "Unable to remove all replay network hooks; runtime restoration={} (dimensionConstructor={}, "
            "LevelChunk={}, SubChunk={}, SetTime={}, resourceHandlers={}, completion={}, packetRead={}, "
            "soundHandlers={})",
            restored,
            state.dimensionConstructor,
            state.levelChunk,
            state.subChunk,
            state.setTime,
            state.resourceHandlersInstalled(),
            state.completion,
            state.packetRead,
            state.soundHandlersInstalled()
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

} // namespace playback::record
