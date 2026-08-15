#include "PacketLifecycle.h"

#include "mc/network/MinecraftPacketIds.h"

#include <array>
#include <cstddef>

namespace playback::packet {

namespace {

struct PacketLifecycleEntry {
    MinecraftPacketIds       packetId;
    PacketLifecycleSemantics semantics;
};

// Unlisted packet IDs are ordinary timeline packets. The table only describes lifecycle exceptions.
constexpr auto PacketLifecycleTable = std::to_array<PacketLifecycleEntry>({
    {MinecraftPacketIds::MoveAbsoluteActor, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::MovePlayer, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::NetworkChunkPublisherUpdate, {PacketLifecycle::Ignore}},
    {MinecraftPacketIds::ChunkRadiusUpdated, {PacketLifecycle::Ignore}},

    {MinecraftPacketIds::ResourcePacksInfo, {PacketLifecycle::PreWorldHandshake, false, true}},
    {MinecraftPacketIds::ResourcePackStack, {PacketLifecycle::PreWorldHandshake}},

    {MinecraftPacketIds::AvailableActorIDList, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::BiomeDefinitionList, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CreativeContent, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::AvailableCommands,
     {PacketLifecycle::SnapshotLatest, true, false, MinecraftPacketIds::UpdateSoftEnum}},
    {MinecraftPacketIds::ItemRegistryPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::SyncActorProperty, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::DimensionDataPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::FeatureRegistryPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CameraPresets, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CompressedBiomeDefinitionListDeprecated, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::TrimData, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::VoxelShapesPacket, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CameraSpline, {PacketLifecycle::SnapshotLatest}},
    {MinecraftPacketIds::CameraAimAssistActorPriority, {PacketLifecycle::SnapshotLatest}},

    {MinecraftPacketIds::CraftingData, {PacketLifecycle::SnapshotSequence}},
    {MinecraftPacketIds::UpdateSoftEnum, {PacketLifecycle::SnapshotSequence}},
    {MinecraftPacketIds::CameraAimAssistPresets, {PacketLifecycle::SnapshotSequence}},
});

consteval bool hasUniquePacketIds() {
    for (std::size_t left = 0; left < PacketLifecycleTable.size(); ++left) {
        for (std::size_t right = left + 1; right < PacketLifecycleTable.size(); ++right) {
            if (PacketLifecycleTable[left].packetId == PacketLifecycleTable[right].packetId) return false;
        }
    }
    return true;
}

static_assert(hasUniquePacketIds(), "Packet lifecycle table contains duplicate packet IDs");

} // namespace

PacketLifecycleSemantics describePacketLifecycle(MinecraftPacketIds packetId) {
    for (auto const& entry : PacketLifecycleTable) {
        if (entry.packetId == packetId) return entry.semantics;
    }
    return {};
}

} // namespace playback::packet
