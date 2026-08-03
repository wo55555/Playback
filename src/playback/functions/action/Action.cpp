#include "Action.h"

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/replay/ReplaySession.h"

#include "mc/network/MinecraftPacketIds.h"

namespace playback::functions {

ConfigurationPacketCachePolicy getConfigurationPacketCachePolicy(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::ResourcePacksInfo:
    case MinecraftPacketIds::ResourcePackStack:
    case MinecraftPacketIds::AvailableActorIDList:
    case MinecraftPacketIds::BiomeDefinitionList:
    case MinecraftPacketIds::CreativeContent:
    case MinecraftPacketIds::AvailableCommands:
    case MinecraftPacketIds::ItemRegistryPacket:
    case MinecraftPacketIds::SyncActorProperty:
    case MinecraftPacketIds::DimensionDataPacket:
    case MinecraftPacketIds::FeatureRegistryPacket:
    case MinecraftPacketIds::CameraPresets:
    case MinecraftPacketIds::CompressedBiomeDefinitionListDeprecated:
    case MinecraftPacketIds::TrimData:
    case MinecraftPacketIds::VoxelShapesPacket:
    case MinecraftPacketIds::CameraSpline:
    case MinecraftPacketIds::CameraAimAssistActorPriority:
        return ConfigurationPacketCachePolicy::Latest;
    case MinecraftPacketIds::CraftingData:
    case MinecraftPacketIds::UpdateSoftEnum:
    case MinecraftPacketIds::CameraAimAssistPresets:
        return ConfigurationPacketCachePolicy::Sequence;
    default:
        return ConfigurationPacketCachePolicy::Ignore;
    }
}

bool shouldReplayConfigurationPacketEverySnapshot(MinecraftPacketIds packetId) {
    return getConfigurationPacketCachePolicy(packetId) == ConfigurationPacketCachePolicy::Sequence
        || packetId == MinecraftPacketIds::AvailableCommands;
}

// ActionNextTick
void ActionNextTick::handle(functions::ReplaySession& session, PlaybackBuffer&) { session.handleNextTick(); }

// ActionSnapshotContext
void ActionSnapshotContext::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSnapshotContext(readSnapshotContext(data));
}

// ActionCreateLocalPlayer
void ActionCreateLocalPlayer::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleCreateLocalPlayer(data);
}

// ActionLevelChunkCached
void ActionLevelChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleLevelChunkCached(data.getVarInt().value());
}

// ActionSubChunkCached
void ActionSubChunkCached::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleSubChunkCached(data.getVarInt().value());
}

// ActionConfigurationPacket
void ActionConfigurationPacket::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleConfigurationPacket(data);
}

// ActionGamePacket
void ActionGamePacket::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleGamePacket(data);
}

// ActionMoveEntities
void ActionMoveEntities::handle(functions::ReplaySession& session, PlaybackBuffer& data) {
    session.handleMoveEntities(data);
}

} // namespace playback::functions
