#include "Recorder.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/common/SubClientId.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/LevelChunkPacket.h"

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

LL_TYPE_INSTANCE_HOOK(
    OnValidPacketReceivedHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$onValidPacketReceived,
    void,
    ::NetworkIdentifier const& netId,
    ::MinecraftPacketIds       packetId,
    ::SubClientId              subId,
    ::SubClientId              recipientSubId
) {
    auto& recorder = functions::Recorder::getInstance();
    // if (!recorder.isPaused()) {
    //     getLogger().debug("[PacketIn] id={}", static_cast<int>(packetId));
    // }
    origin(netId, packetId, subId, recipientSubId);
}

LL_TYPE_INSTANCE_HOOK(
    LevelChunkHook,
    ll::memory::HookPriority::Normal,
    LegacyClientNetworkHandler,
    &LegacyClientNetworkHandler::$handle,
    void,
    ::NetworkIdentifier const&            source,
    ::std::shared_ptr<::LevelChunkPacket> packet
) {
    auto& recorder = functions::Recorder::getInstance();
    if (packet) {
        getLogger().debug(
            "[LevelChunk] pos=({},{}) dim={}",
            packet->mPos->x,
            packet->mPos->z,
            static_cast<int>(packet->mDimensionId->id)
        );
    }

    if (!recorder.isPaused() && packet) {
        // TODO: 将包内容序列化为二进制，存储到 Recorder
    }
    origin(source, packet);
}

using OnValidPacketReceivedHookReg = ll::memory::HookRegistrar<OnValidPacketReceivedHook>;
using LevelChunkHookReg            = ll::memory::HookRegistrar<LevelChunkHook>;

void hookNetwork(bool enable) {
    if (enable) {
        OnValidPacketReceivedHookReg::hook();
        LevelChunkHookReg::hook();
    } else {
        OnValidPacketReceivedHookReg::unhook();
        LevelChunkHookReg::unhook();
    }
}

} // namespace playback::functions
