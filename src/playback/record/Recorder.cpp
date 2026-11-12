#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/action/Action.h"
#include "playback/io/AsyncReplaySaver.h"
#include "playback/io/ReplayResourcePacks.h"
#include "playback/keyframe/ClientCameraCapture.h"
#include "playback/packet/PacketLifecycle.h"
#include "playback/record/ChunkMutationBarrier.h"
#include "playback/utils/MapItemId.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/common/Common.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "mc/entity/components/VanillaOffsetComponent.h"
#include "mc/entity/systems/VanillaOffsetSystem.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/ActorEventPacket.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/BlockActorDataPacket.h"
#include "mc/network/packet/BlockEventPacket.h"
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/ClientboundMapItemDataPacket.h"
#include "mc/network/packet/DimensionDataPacket.h"
#include "mc/network/packet/InventorySlotPacket.h"
#include "mc/network/packet/ItemData.h"
#include "mc/network/packet/ItemRegistryPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/LevelSoundEventPacket.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MobEffectPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerHotbarPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketType.h"
#include "mc/network/packet/PlayerSkinPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/ScorePacketInfo.h"
#include "mc/network/packet/SetActorDataPacket.h"
#include "mc/network/packet/SetActorLinkPacket.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/SetDisplayObjectivePacket.h"
#include "mc/network/packet/SetScorePacket.h"
#include "mc/network/packet/SetSpawnPositionPacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/TakeItemActorPacket.h"
#include "mc/network/packet/UpdateAttributesPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdatePlayerGameTypePacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/network/packet/UpdateSubChunkNetworkBlockInfo.h"
#include "mc/util/VarIntDataOutput.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerInventory.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/state/PropertyComponent.h"
#include "mc/world/attribute/Attribute.h"
#include "mc/world/attribute/AttributeData.h"
#include "mc/world/attribute/AttributeInstance.h"
#include "mc/world/attribute/AttributeInstanceConstRef.h"
#include "mc/world/attribute/AttributeInstanceHandle.h"
#include "mc/world/attribute/AttributeModifier.h"
#include "mc/world/attribute/SharedAttributes.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/MapItem.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/BlockPalette.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BedrockBlockNames.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/saveddata/maps/MapItemSavedData.h"
#include "mc/world/level/storage/LevelData.h"
#include "mc/world/scores/DisplayObjective.h"
#include "mc/world/scores/Objective.h"
#include "mc/world/scores/ObjectiveCriteria.h"
#include "mc/world/scores/Scoreboard.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include <uuid.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iterator>
#include <latch>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

PlayerInputTick::PlayerInputTick() { mValue = 0; }

namespace playback::record {
using namespace playback::action;
using namespace playback::packet;
using namespace playback::io;

namespace {

constexpr ActorUniqueID  RecordedPlayerUniqueId{std::numeric_limits<int64_t>::max() - 1024};
constexpr ActorRuntimeID RecordedPlayerRuntimeId{RecordedPlayerRuntimeIdValue};

// A dimension change floods the engine task groups, and a missed snapshot there cancels the whole recording.
constexpr auto DimensionChangeBarrierTimeout = std::chrono::milliseconds{15000};
constexpr auto ChunkRotationBarrierTimeout   = std::chrono::milliseconds{8000};
constexpr auto InitialSnapshotBarrierTimeout = std::chrono::milliseconds{2000};
// The longest the snapshot of a new dimension waits for the game's loading screen to go.
constexpr auto DimensionLoadingTimeout = std::chrono::seconds{30};

// Client thread time one rotation slice may block a tick for.
constexpr uint64_t RotationSliceBudgetNs = 8'000'000;

uint64_t steadyNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count()
    );
}

uint64_t mixFingerprint(uint64_t hash, uint64_t value) {
    return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

uint64_t packDirtyTicks(DirtyTicksCounter const& counter) {
    return static_cast<uint32_t>(counter.totalTime) | (uint64_t{static_cast<uint32_t>(counter.lastChange)} << 32);
}

// Engine state that moves when a subchunk's blocks change or its block storage is replaced.
uint64_t fingerprintSubChunks(LevelChunk const& chunk) {
    auto const& subChunks = *chunk.mSubChunks;
    uint64_t    hash      = subChunks.size();
    for (auto const& subChunk : subChunks) {
        auto const& blocks = subChunk.mBlocksReadPtr;
        hash               = mixFingerprint(hash, packDirtyTicks(*subChunk.mDirtyTicksCounter));
        hash               = mixFingerprint(hash, static_cast<uint64_t>(subChunk.mSubChunkState));
        hash               = mixFingerprint(hash, reinterpret_cast<uintptr_t>(blocks[0]));
        hash               = mixFingerprint(hash, reinterpret_cast<uintptr_t>(blocks[1]));
        hash               = mixFingerprint(hash, subChunk.mHash);
        hash               = mixFingerprint(hash, static_cast<uint64_t>(subChunk.mHashDirty));
        hash               = mixFingerprint(hash, static_cast<uint64_t>(static_cast<uint8_t>(subChunk.mAbsoluteIndex)));
    }
    return hash;
}

uint64_t fingerprintChunk(LevelChunk const& chunk) {
    uint64_t hash = 0;
    for (auto const& counter : chunk.mFullChunkDirtyTicksCounters) {
        hash = mixFingerprint(hash, packDirtyTicks(counter.get()));
    }
    return hash;
}

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

// The replay observer's HUD and inventory screen read its own slots. The whole player inventory is recorded, not
// just the hotbar, so the backpack rows match the recording. HudCacheSize must fit Recorder.h mLastLocalHudPackets.
constexpr int    HudHotbarSlots  = 36;
constexpr int    HudArmorSlots   = 4;
constexpr size_t HudArmorKey     = HudHotbarSlots;
constexpr size_t HudOffhandKey   = HudArmorKey + HudArmorSlots;
constexpr size_t HudAttributeKey = HudOffhandKey + 1;
constexpr size_t HudSelectedKey  = HudAttributeKey + 1;
constexpr size_t HudCacheSize    = HudSelectedKey + 1;
static_assert(HudCacheSize <= 48);
static_assert(HudOffhandKey < 41);

std::shared_ptr<Packet> makeHudSlotPacket(ContainerID container, int slot, ItemStack const& item) {
    return std::make_shared<InventorySlotPacket>(InventorySlotPacketPayload(container, slot, item));
}

std::shared_ptr<Packet> makeHudSelectedSlotPacket(Player& player) {
    return std::make_shared<PlayerHotbarPacket>(
        PlayerHotbarPacketPayload(static_cast<uint>(player.getSelectedItemSlot()), ContainerID::Inventory, true)
    );
}

// Only what the HUD draws; exhaustion and the like change every tick and would bloat the recording.
std::shared_ptr<Packet> makeHudAttributePacket(Player& player, ActorRuntimeID runtimeId) {
    // AttributeData cannot be copied or moved from here, so the SDK constructor builds the entries from handles.
    auto const attributes = {
        &SharedAttributes::HEALTH(),
        &SharedAttributes::ABSORPTION(),
        &Player::HUNGER(),
        &Player::SATURATION(),
        &Player::LEVEL(),
        &Player::EXPERIENCE(),
    };
    std::vector<AttributeInstanceHandle> handles;
    handles.reserve(attributes.size());
    for (auto const* attribute : attributes) {
        auto const instance = player.getAttribute(*attribute);
        if (instance.mPtr) handles.push_back(instance.mPtr->getHandle());
    }
    auto packet        = std::make_shared<UpdateAttributesPacket>(player, handles);
    packet->mRuntimeId = runtimeId;
    return packet;
}

std::shared_ptr<Packet> makeHudGameTypePacket(Player& player, ActorUniqueID target) {
    auto packet             = std::make_shared<UpdatePlayerGameTypePacket>();
    packet->mPlayerGameType = player.getPlayerGameType();
    packet->mTargetPlayer   = target;
    packet->mTick           = PlayerInputTick{};
    return packet;
}

std::optional<int64_t> heldMapId(ItemStack const& item) { return utils::readMapUuid(item); }

// What a slot packet serializes. Same content means the packet bytes cannot have changed.
bool sameHudItem(ItemStack const& item, ItemStack const& previous) {
    return item.mCount == previous.mCount && item.mBlock == previous.mBlock && item.getId() == previous.getId()
        && item.getAuxValue() == previous.getAuxValue() && item.hasSameUserData(previous);
}

// The replay level only has the map data its recorded map packets carried, so a held map is written out in full.
std::unique_ptr<Packet> makeHeldMapPacket(Player& player, int64_t mapId) {
    auto* data = player.getLevel().getMapSavedData(ActorUniqueID{mapId});
    return data ? data->getFullDataPacket() : nullptr;
}

bool isSuccessfulSubChunkResult(SubChunkPacket::SubChunkRequestResult result) {
    return result == SubChunkPacket::SubChunkRequestResult::Success
        || result == SubChunkPacket::SubChunkRequestResult::SuccessAllAir;
}

bool remapRuntimeId(ActorRuntimeID& id, ActorRuntimeID source, ActorRuntimeID target) {
    if (id.rawID != source.rawID) return false;
    id = target;
    return true;
}

bool remapUniqueId(ActorUniqueID& id, ActorUniqueID source, ActorUniqueID target) {
    if (id.rawID != source.rawID) return false;
    id = target;
    return true;
}

bool packetCarriesBlockNetworkIds(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::UpdateBlock:
    case MinecraftPacketIds::UpdateBlockSynced:
    case MinecraftPacketIds::UpdateSubChunkBlocks:
        return true;
    default:
        return false;
    }
}

// Sequential palette indices only mean something on the recording server, so store the portable hash instead.
bool portableBlockNetworkId(uint& runtimeId, BlockPalette const& palette) {
    auto const& sequentialBlocks = *palette.mBlockFromNetworkId;
    if (runtimeId >= sequentialBlocks.size()) return false;

    auto const* block = sequentialBlocks[runtimeId];
    if (!block || block->mSerializationIdHashForNetwork == 0) return false;
    if (runtimeId == block->mSerializationIdHashForNetwork) return false;

    runtimeId = block->mSerializationIdHashForNetwork;
    return true;
}

bool makeBlockNetworkIdsPortable(Packet& packet, BlockPalette const& palette) {
    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock:
        return portableBlockNetworkId(static_cast<UpdateBlockPacket&>(packet).mRuntimeId, palette);
    case MinecraftPacketIds::UpdateBlockSynced:
        return portableBlockNetworkId(static_cast<UpdateBlockSyncedPacket&>(packet).mRuntimeId, palette);
    case MinecraftPacketIds::UpdateSubChunkBlocks: {
        auto& changed = *static_cast<UpdateSubChunkBlocksPacket&>(packet).mBlocksChanged;
        bool  updated = false;
        for (auto* blocks : {&*changed.mStandards, &*changed.mExtras}) {
            for (auto& info : *blocks) {
                updated |= portableBlockNetworkId(info.mRuntimeId, palette);
            }
        }
        return updated;
    }
    default:
        return false;
    }
}

bool packetMayReferenceRecordedPlayer(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::AddPlayer:
    case MinecraftPacketIds::PlayerList:
    case MinecraftPacketIds::PlayerSkin:
    case MinecraftPacketIds::RemoveActor:
    case MinecraftPacketIds::TakeItemActor:
    case MinecraftPacketIds::ActorEvent:
    case MinecraftPacketIds::MobEffect:
    case MinecraftPacketIds::UpdateAttributes:
    case MinecraftPacketIds::PlayerEquipment:
    case MinecraftPacketIds::MobArmorEquipment:
    case MinecraftPacketIds::SetActorData:
    case MinecraftPacketIds::SetActorMotion:
    case MinecraftPacketIds::SetActorLink:
    case MinecraftPacketIds::Animate:
    case MinecraftPacketIds::PlayerAction:
    case MinecraftPacketIds::LevelSoundEvent:
    case MinecraftPacketIds::UpdatePlayerGameType:
        return true;
    default:
        return false;
    }
}

// Mirrors remapRecordedPlayerReferences on the live packet, so packets about other actors skip the decode round trip.
bool referencesRecordedPlayer(
    Packet const&    packet,
    ActorUniqueID    sourceUniqueId,
    ActorRuntimeID   sourceRuntimeId,
    mce::UUID const& sourceUuid
) {
    auto const runtime = [&sourceRuntimeId](ActorRuntimeID const& id) { return id.rawID == sourceRuntimeId.rawID; };
    auto const unique  = [&sourceUniqueId](ActorUniqueID const& id) { return id.rawID == sourceUniqueId.rawID; };
    switch (packet.getId()) {
    case MinecraftPacketIds::AddPlayer: {
        auto const& addPlayer = static_cast<AddPlayerPacket const&>(packet);
        if (unique(addPlayer.mEntityId) || runtime(addPlayer.mRuntimeId) || *addPlayer.mUuid == sourceUuid) {
            return true;
        }
        return std::any_of(addPlayer.mLinks->begin(), addPlayer.mLinks->end(), [&unique](ActorLink const& link) {
            return unique(link.A) || unique(link.B);
        });
    }
    case MinecraftPacketIds::PlayerList: {
        auto const& entries = *static_cast<PlayerListPacket const&>(packet).mEntries;
        return std::any_of(entries.begin(), entries.end(), [&](PlayerListEntry const& entry) {
            return unique(entry.mId) || *entry.mUUID == sourceUuid;
        });
    }
    case MinecraftPacketIds::RemoveActor:
        return unique(static_cast<RemoveActorPacket const&>(packet).mEntityId);
    case MinecraftPacketIds::TakeItemActor: {
        auto const& takeItem = static_cast<TakeItemActorPacket const&>(packet);
        return runtime(takeItem.mItemId) || runtime(takeItem.mActorId);
    }
    case MinecraftPacketIds::ActorEvent:
        return runtime(static_cast<ActorEventPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::MobEffect:
        return runtime(static_cast<MobEffectPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::UpdateAttributes:
        return runtime(static_cast<UpdateAttributesPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::PlayerEquipment:
        return runtime(static_cast<MobEquipmentPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::MobArmorEquipment:
        return runtime(static_cast<MobArmorEquipmentPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::SetActorData:
        return runtime(static_cast<SetActorDataPacket const&>(packet).mId);
    case MinecraftPacketIds::SetActorMotion:
        return runtime(static_cast<SetActorMotionPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::SetActorLink: {
        auto const& link = *static_cast<SetActorLinkPacket const&>(packet).mLink;
        return unique(link.A) || unique(link.B);
    }
    case MinecraftPacketIds::Animate:
        return runtime(static_cast<AnimatePacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::PlayerAction:
        return runtime(static_cast<PlayerActionPacket const&>(packet).mRuntimeId);
    case MinecraftPacketIds::LevelSoundEvent:
        return unique(static_cast<LevelSoundEventPacket const&>(packet).mActor);
    case MinecraftPacketIds::UpdatePlayerGameType:
        return unique(static_cast<UpdatePlayerGameTypePacket const&>(packet).mTargetPlayer);
    default:
        return true;
    }
}

bool remapRecordedPlayerReferences(
    Packet&          packet,
    ActorUniqueID    sourceUniqueId,
    ActorRuntimeID   sourceRuntimeId,
    mce::UUID const& sourceUuid,
    ActorUniqueID    targetUniqueId,
    ActorRuntimeID   targetRuntimeId,
    mce::UUID const& targetUuid
) {
    switch (packet.getId()) {
    case MinecraftPacketIds::AddPlayer: {
        auto& addPlayer = static_cast<AddPlayerPacket&>(packet);
        bool  changed   = addPlayer.mEntityId->rawID == sourceUniqueId.rawID
                    || addPlayer.mRuntimeId->rawID == sourceRuntimeId.rawID || *addPlayer.mUuid == sourceUuid;
        for (auto& link : *addPlayer.mLinks) {
            changed |= remapUniqueId(link.A, sourceUniqueId, targetUniqueId);
            changed |= remapUniqueId(link.B, sourceUniqueId, targetUniqueId);
        }
        if (!changed) return false;
        addPlayer.mUuid      = targetUuid;
        addPlayer.mEntityId  = targetUniqueId;
        addPlayer.mRuntimeId = targetRuntimeId;
        addPlayer.mPlatformOnlineId->clear();
        addPlayer.mDeviceId->clear();
        return true;
    }
    case MinecraftPacketIds::PlayerList: {
        bool  changed    = false;
        auto& playerList = static_cast<PlayerListPacket&>(packet);
        for (auto& entry : *playerList.mEntries) {
            if (entry.mId->rawID != sourceUniqueId.rawID && *entry.mUUID != sourceUuid) continue;
            entry.mId   = targetUniqueId;
            entry.mUUID = targetUuid;
            entry.mXUID->clear();
            entry.mPlatformOnlineId->clear();
            changed = true;
        }
        return changed;
    }
    case MinecraftPacketIds::PlayerSkin: {
        auto& skinPacket = static_cast<PlayerSkinPacket&>(packet);
        if (*skinPacket.mUUID != sourceUuid) return false;
        skinPacket.mUUID = targetUuid;
        return true;
    }
    case MinecraftPacketIds::RemoveActor:
        return remapUniqueId(static_cast<RemoveActorPacket&>(packet).mEntityId, sourceUniqueId, targetUniqueId);
    case MinecraftPacketIds::TakeItemActor: {
        auto& takeItem  = static_cast<TakeItemActorPacket&>(packet);
        bool  changed   = remapRuntimeId(takeItem.mItemId, sourceRuntimeId, targetRuntimeId);
        changed        |= remapRuntimeId(takeItem.mActorId, sourceRuntimeId, targetRuntimeId);
        return changed;
    }
    case MinecraftPacketIds::ActorEvent:
        return remapRuntimeId(static_cast<ActorEventPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::MobEffect:
        return remapRuntimeId(static_cast<MobEffectPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::UpdateAttributes:
        return remapRuntimeId(
            static_cast<UpdateAttributesPacket&>(packet).mRuntimeId,
            sourceRuntimeId,
            targetRuntimeId
        );
    case MinecraftPacketIds::PlayerEquipment:
        return remapRuntimeId(static_cast<MobEquipmentPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::MobArmorEquipment:
        return remapRuntimeId(
            static_cast<MobArmorEquipmentPacket&>(packet).mRuntimeId,
            sourceRuntimeId,
            targetRuntimeId
        );
    case MinecraftPacketIds::SetActorData:
        return remapRuntimeId(static_cast<SetActorDataPacket&>(packet).mId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::SetActorMotion:
        return remapRuntimeId(static_cast<SetActorMotionPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::SetActorLink: {
        auto& link     = *static_cast<SetActorLinkPacket&>(packet).mLink;
        bool  changed  = remapUniqueId(link.A, sourceUniqueId, targetUniqueId);
        changed       |= remapUniqueId(link.B, sourceUniqueId, targetUniqueId);
        return changed;
    }
    case MinecraftPacketIds::Animate:
        return remapRuntimeId(static_cast<AnimatePacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::PlayerAction:
        return remapRuntimeId(static_cast<PlayerActionPacket&>(packet).mRuntimeId, sourceRuntimeId, targetRuntimeId);
    case MinecraftPacketIds::LevelSoundEvent:
        return remapUniqueId(static_cast<LevelSoundEventPacket&>(packet).mActor, sourceUniqueId, targetUniqueId);
    case MinecraftPacketIds::UpdatePlayerGameType:
        return remapUniqueId(
            static_cast<UpdatePlayerGameTypePacket&>(packet).mTargetPlayer,
            sourceUniqueId,
            targetUniqueId
        );
    default:
        return false;
    }
}

std::string sanitizeFileName(std::string name) {
    for (auto& ch : name) {
        auto byte = static_cast<unsigned char>(ch);
        if (byte < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|'
            || ch == '?' || ch == '*') {
            ch = '_';
        }
    }

    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }

    return name.empty() ? "replay" : name;
}

std::string currentReplayTimestampName() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%dT%H-%M-%S");
    return stream.str();
}

std::string uuidReplayName() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return uuids::to_string(id);
}

std::string findAvailableReplayName(std::filesystem::path const& replayDir, std::string baseName) {
    constexpr std::string_view extension = ".zip";

    baseName = sanitizeFileName(std::move(baseName));

    for (int index = 0; index < 10000; ++index) {
        std::string filename = baseName;
        if (index > 0) {
            filename += " (" + std::to_string(index) + ")";
        }
        filename += extension;

        std::error_code ec;
        bool            exists = std::filesystem::exists(replayDir / filename, ec);
        if (ec) {
            getLogger().error("Error while trying to determine replay filename: {}", ec.message());
            break;
        }
        if (!exists) {
            return filename;
        }
    }

    return uuidReplayName() + std::string(extension);
}

std::string
snapshotFailure(ChunkPos const& pos, std::optional<int> subChunkY, std::string_view stage, std::string_view reason) {
    std::ostringstream stream;
    stream << "Chunk snapshot failed at column (" << pos.x << ", " << pos.z << ')';
    if (subChunkY) stream << ", subchunk Y " << *subChunkY;
    stream << " during " << stage << ": " << reason;
    return stream.str();
}

nlohmann::ordered_json chunkMetaToJson(PlaybackChunkMeta const& meta) {
    return {
        {"duration",          meta.duration         },
        {"forcePlaySnapshot", meta.forcePlaySnapshot}
    };
}

nlohmann::ordered_json metaToJson(PlaybackMeta const& meta) {
    auto chunks = nlohmann::ordered_json::object();
    for (auto const& [chunkName, chunkMeta] : meta.chunks) {
        chunks[chunkName] = chunkMetaToJson(chunkMeta);
    }

    nlohmann::ordered_json json{
        {"name",        meta.name        },
        {"worldName",   meta.worldName   },
        {"totalTicks",  meta.totalTicks  },
        {"gameVersion", meta.gameVersion },
        {"chunks",      std::move(chunks)}
    };

    return json;
}

PlaybackChunkMeta chunkMetaFromJson(nlohmann::ordered_json const& json) {
    return PlaybackChunkMeta{json.at("duration").get<int>(), json.at("forcePlaySnapshot").get<bool>()};
}

PlaybackMeta metaFromJson(nlohmann::ordered_json const& json) {
    PlaybackMeta meta;
    meta.name       = json.at("name").get<std::string>();
    meta.worldName  = json.at("worldName").get<std::string>();
    meta.totalTicks = json.at("totalTicks").get<int>();
    if (auto const it = json.find("gameVersion"); it != json.end() && it->is_string()) {
        meta.gameVersion = it->get<std::string>();
    }

    auto const& chunks = json.at("chunks");
    if (!chunks.is_object()) {
        throw std::invalid_argument("Playback metadata chunks must be an object");
    }

    for (auto it = chunks.begin(); it != chunks.end(); ++it) {
        meta.chunks.insert_or_assign(it.key(), chunkMetaFromJson(it.value()));
    }
    return meta;
}

// The vanilla camera sits below the entity position while sneaking or swimming; the replayed actor has no such state.
PlaybackCameraSample captureLocalCameraSample(LocalPlayer& player) {
    Vec3 offset{0.0f, 0.0f, 0.0f};
    if (player.getEntityContext().hasComponent<VanillaOffsetComponent>()) {
        auto const camera   = VanillaOffsetSystem::getCameraPosition(player, 1.0f);
        auto const position = player.getPosition();
        offset              = {camera.x - position.x, camera.y - position.y, camera.z - position.z};
    }
    PlaybackCameraSample sample{offset.x, offset.y, offset.z, latestRenderedFov()};
    // While sleeping the vanilla camera detaches from the entity, so its absolute pose is recorded instead.
    if (player.isSleeping()) {
        bool foreignCamera = false;
        if (auto client = ll::service::getClientInstance()) {
            auto* cameraActor = client->getCameraActor();
            foreignCamera     = cameraActor && cameraActor != &player;
        }
        // The sleep camera system overrides every preset's pose, so a third-person sleep still carries the bed view;
        // only a /camera view is not the player's own.
        if (!foreignCamera) {
            if (auto const view = keyframe::captureClientCamera()) {
                sample.hasView   = true;
                sample.viewX     = view->x;
                sample.viewY     = view->y;
                sample.viewZ     = view->z;
                sample.viewYaw   = view->yaw;
                sample.viewPitch = view->pitch;
            }
        }
    }
    return sample;
}

} // namespace

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto j = nlohmann::ordered_json::parse(json);
    return metaFromJson(j);
}

std::string PlaybackMeta::currentGameVersion() { return Common::getGameVersionString(); }

bool PlaybackMeta::isCompatibleWithRuntime() const {
    // Files recorded before the field existed cannot be verified; let them through.
    return gameVersion.empty() || gameVersion == currentGameVersion();
}

std::string PlaybackMeta::toJson() const { return metaToJson(*this).dump(); }

Recorder::Recorder() : mSnapshotLevelChunks{}, mSnapshotSubChunks{} {
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
}

void Recorder::start() {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        getLogger().error("Recording requires a local player");
        return;
    }

    if (mState.load() == State::Paused) {
        mState = State::Recording;
        return;
    }

    auto& playback = playback::Playback::getInstance();
    if (playback.isReplayMode()) {
        mState = State::Idle;
        return;
    }

    if (mState.load() != State::Idle) {
        return;
    }

    // The pending recording still owns the pack worker that a new recording would replace, so it is saved first.
    if (hasPendingRecording()) {
        if (!savePendingRecording({})) {
            getLogger().error("Unable to save the finished recording; keeping it until it is saved or deleted");
            return;
        }
        getLogger().info("Saved the unnamed finished recording before starting a new one");
    }

    resetStateForNewRecording();
    mRecordedLocalPlayerId        = RecordedPlayerUniqueId;
    mRecordedLocalPlayerRuntimeId = RecordedPlayerRuntimeId;
    mRecordedLocalPlayerUuid      = mce::UUID::random();
    if (auto error = mAsyncReplaySaver->getError()) {
        cancelRecording(*error);
        return;
    }

    if (!hookNetwork(true)) {
        cancelRecording("Required replay network hooks are unavailable");
        return;
    }

    mState = State::Recording;
    getLogger().info("Recording started");
}

void Recorder::pause() {
    if (mState.load() != State::Recording) return;
    mState = State::Paused;
}

void Recorder::stop() {
    if (hasPendingRecording() && !savePendingRecording({})) {
        getLogger().error("Unsaved replay data was left at {}", mPendingRecordPath);
        clearPendingRecording();
    }
    if (!closeRecording()) return;
    auto replayPath = finishSaver();
    if (replayPath.empty()) return;
    if (!exportRecording(replayPath, currentReplayTimestampName(), {})) {
        getLogger().error("Failed to save replay data after recording stopped");
    }
}

void Recorder::discard() {
    auto const state = mState.load();
    if (state != State::Recording && state != State::Paused) return;
    getLogger().info("Recording discarded");
    abortRecording();
}

bool Recorder::finishToPending() {
    if (hasPendingRecording()) return false;
    if (!closeRecording()) return false;
    auto replayPath = finishSaver();
    if (replayPath.empty()) return false;
    mPendingRecordPath    = std::move(replayPath);
    mPendingRecordingName = currentReplayTimestampName();
    mHasPendingRecording.store(true, std::memory_order_release);
    return true;
}

bool Recorder::savePendingRecording(std::string_view name) {
    if (!hasPendingRecording()) return false;
    auto const  first = name.find_first_not_of(" \t");
    auto const  last  = name.find_last_not_of(" \t");
    std::string trimmed =
        first == std::string_view::npos ? std::string{} : std::string(name.substr(first, last - first + 1));
    if (!exportRecording(mPendingRecordPath, trimmed.empty() ? mPendingRecordingName : trimmed, trimmed)) {
        getLogger().error("Failed to save the finished recording");
        return false;
    }
    clearPendingRecording();
    return true;
}

void Recorder::discardPendingRecording() {
    if (!hasPendingRecording()) return;
    if (mPackWorker) {
        mPackWorker->cancel();
        mPackWorker.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(mPendingRecordPath, ec);
    if (ec) getLogger().error("Failed to delete discarded replay data {}: {}", mPendingRecordPath, ec.message());
    else getLogger().info("Finished recording deleted");
    clearPendingRecording();
}

void Recorder::clearPendingRecording() {
    mPendingRecordPath.clear();
    mPendingRecordingName.clear();
    mHasPendingRecording.store(false, std::memory_order_release);
}

bool Recorder::closeRecording() {
    State state = mState.exchange(State::Closing);
    if (state == State::Idle) {
        mState = State::Idle;
        return false;
    }
    if (state == State::Closing) {
        return false;
    }

    endTick(true);
    // endTick returns early without a local player, before it drops a rotation snapshot still in progress.
    discardRotationSnapshot();
    if (mState.load() != State::Closing) return false;
    if (mNeedsInitialSnapshot.load(std::memory_order_acquire)) {
        auto reason = mSnapshotFailure.empty()
                        ? std::string("Recording stopped before the initial replay snapshot was ready")
                        : "Recording stopped without an initial replay snapshot: " + mSnapshotFailure;
        cancelRecording(reason);
        return false;
    }
    logRecordedGamePacketSummary();
    getLogger().debug(
        "Recorded local view samples: rate={} ticks={}/{} samples={}",
        mLocalViewRate,
        mLocalViewTicks,
        mWrittenTicks,
        mLocalViewSamples
    );
    return true;
}

std::filesystem::path Recorder::finishSaver() {
    if (!mAsyncReplaySaver) {
        getLogger().error("Failed to stop recording because replay saver is not initialized");
        mState = State::Idle;
        return {};
    }

    auto replayPath = mAsyncReplaySaver->finish();
    auto saverError = mAsyncReplaySaver->getError();
    mAsyncReplaySaver.reset();
    mState = State::Idle;

    if (saverError || replayPath.empty()) {
        if (mPackWorker) {
            mPackWorker->cancel();
            mPackWorker.reset();
        }
        getLogger().error(
            "Failed to save recording: {}",
            saverError.value_or("the replay saver did not return a completed recording path")
        );
        return {};
    }

    auto* thumbnailProvider = mThumbnailCaptureProvider.load(std::memory_order_acquire);
    if (!mThumbnailCaptureRequested || !thumbnailProvider
        || !thumbnailProvider->saveReplayThumbnail(replayPath / "icon.png")) {
        getLogger().warn("Unable to save replay thumbnail for {}", replayPath);
    }
    return replayPath;
}

bool Recorder::exportRecording(
    std::filesystem::path const& replayPath,
    std::string const&           fileBaseName,
    std::string_view             metaName
) {
    auto            replayDir = utils::PathUtils::getReplaysDir();
    std::error_code ec;
    std::filesystem::create_directories(replayDir, ec);
    if (ec) {
        getLogger().error("Error while trying to create replay folder: {}", ec.message());
        return false;
    }

    // Only one export at a time, so this one cannot pick the file name of a zip that is still being written.
    waitForExport();
    auto       outputPath    = replayDir / findAvailableReplayName(replayDir, fileBaseName);
    bool const completePacks = Playback::getInstance().getConfig().record.embedServerPacks;
    auto       packWorker    = std::move(mPackWorker);
    mExportThread            = std::jthread([replayPath,
                                  outputPath,
                                  completePacks,
                                  name       = std::string(metaName),
                                  packWorker = std::move(packWorker)]() mutable {
        auto const startedAt = std::chrono::steady_clock::now();
        try {
            if (packWorker) {
                packWorker->finish();
                packWorker.reset();
            }
            if (completePacks) (void)packs::completeServerPacks(replayPath);
            if (!ReplayExporter::exportReplay(replayPath, outputPath, name)) {
                getLogger().error("Failed to save replay data after recording stopped");
                return;
            }
            getLogger().info(
                "Exported replay {} in {:.1f} ms",
                outputPath.filename(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count()
            );
        } catch (std::exception const& exception) {
            getLogger().error("Failed to save replay data after recording stopped: {}", exception.what());
        } catch (...) {
            getLogger().error("Failed to save replay data after recording stopped");
        }
    });
    return true;
}

void Recorder::waitForExport() {
    if (mExportThread.joinable()) mExportThread.join();
}

void Recorder::releaseWorkerPool() { mWorkerPool.reset(); }

namespace {
// One worker per core; the calling thread takes the last share.
size_t workerPoolThreads() { return std::max(1u, std::thread::hardware_concurrency()) - 1; }
} // namespace

void Recorder::runParallel(size_t helpers, std::function<void()> const& work) {
    helpers = std::min(helpers, workerPoolThreads());
    if (helpers > 0 && !mWorkerPool) {
        mWorkerPool = std::make_unique<ll::thread::ThreadPoolExecutor>("playback_worker", workerPoolThreads());
    }
    // Shared so a task can still touch the latch after the waiter has returned.
    auto const done = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(helpers));
    for (size_t submitted = 0; submitted < helpers; ++submitted) {
        try {
            mWorkerPool->execute([&work, done] {
                struct CountDown {
                    std::latch& latch;
                    ~CountDown() { latch.count_down(); }
                } countDown{*done};
                work();
            });
        } catch (...) {
            done->count_down(static_cast<std::ptrdiff_t>(helpers - submitted));
            break;
        }
    }
    try {
        work();
    } catch (...) {
        done->wait();
        throw;
    }
    done->wait();
}

void Recorder::logRecordedGamePacketSummary() const {
    std::scoped_lock lock(mPendingGamePacketsMutex);
    auto             count = [this](MinecraftPacketIds packetId) {
        auto const it = mRecordedGamePacketCounts.find(static_cast<int32_t>(packetId));
        return it == mRecordedGamePacketCounts.end() ? uint64_t{} : it->second;
    };
    getLogger().debug(
        "Recorded timeline packet summary: AddActor={}, AddItemActor={}, RemoveActor={}, TakeItemActor={}, "
        "ActorEvent={}, LevelEvent={}, MobEquipment={}, MobArmorEquipment={}, SetActorData={}, UpdateBlock={}, "
        "UpdateBlockSynced={}, UpdateSubChunkBlocks={}, ChangeDimension={}, LevelChunk={}, SubChunk={}",
        count(MinecraftPacketIds::AddActor),
        count(MinecraftPacketIds::AddItemActor),
        count(MinecraftPacketIds::RemoveActor),
        count(MinecraftPacketIds::TakeItemActor),
        count(MinecraftPacketIds::ActorEvent),
        count(MinecraftPacketIds::LevelEvent),
        count(MinecraftPacketIds::PlayerEquipment),
        count(MinecraftPacketIds::MobArmorEquipment),
        count(MinecraftPacketIds::SetActorData),
        count(MinecraftPacketIds::UpdateBlock),
        count(MinecraftPacketIds::UpdateBlockSynced),
        count(MinecraftPacketIds::UpdateSubChunkBlocks),
        count(MinecraftPacketIds::ChangeDimension),
        count(MinecraftPacketIds::FullChunkData),
        count(MinecraftPacketIds::SubChunkPacket)
    );
}

void Recorder::resetStateForNewRecording() {
    mAsyncReplaySaver = std::make_unique<AsyncReplaySaver>();
    if (Playback::getInstance().getConfig().record.embedServerPacks) {
        mPackWorker = std::make_unique<packs::PackEmbedWorker>(mAsyncReplaySaver->getRecordPath());
    } else {
        mPackWorker.reset();
    }

    mMetadata.chunks.clear();
    mMetadata.totalTicks  = 0;
    mMetadata.gameVersion = PlaybackMeta::currentGameVersion();
    mRecordingDimension.reset();
    discardRotationSnapshot();
    resetChunkSnapshot();
    resetSnapshotColumnCache();
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
    mChunkIndex                    = 0;
    mTicksInCurrentChunk           = 0;
    mWrittenTicks                  = 0;
    mLongestSnapshotStall          = {};
    mHasOpenChunk                  = false;
    mOpenChunkHasData              = false;
    mCurrentChunkForcePlaySnapshot = false;
    mThumbnailCaptureRequested     = false;
    mThumbnailRequestTick          = 0;
    mPendingDimensionSnapshot      = false;
    mNeedsInitialSnapshot          = true;
    mDimensionTransitionPending    = false;
    mDimensionTransitionTargetId   = 0;
    mLastEntityMovements.clear();
    discardLocalViewFrames();
    mLocalViewTicks   = 0;
    mLocalViewSamples = 0;
    mLocalViewRate    = configuration::clampLocalPlayerUpdatesPerSecond(
        Playback::getInstance().getConfig().record.localPlayerUpdatesPerSecond
    );
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    mLocalFormOpen = false;
    mLastLocalHudPackets.fill(std::nullopt);
    mLastLocalHudItems.fill(ItemStack());
    mLastLocalSelectedSlot         = -1;
    mLastLocalAttributeValuesValid = false;
    mLastLocalGameTypeValue        = -1;
    mLastLocalGameTypePacket.reset();
    mLastLocalHeldMapIds.fill(std::nullopt);
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
        mDeferredCompletedChunks.clear();
        mRecordedGamePacketCounts.clear();
        // A registry built from the previous session's level must not be reused after a server change.
        if (mItemRegistrySource == ItemRegistrySource::Synthetic) {
            auto const itemRegistryId = static_cast<int32_t>(MinecraftPacketIds::ItemRegistryPacket);
            std::erase_if(mConfigurationPackets, [itemRegistryId](auto const& cached) {
                return cached.mPacketId == itemRegistryId;
            });
            mConfigurationPacketIndices.clear();
            for (size_t index = 0; index < mConfigurationPackets.size(); ++index) {
                mConfigurationPacketIndices.insert_or_assign(mConfigurationPackets[index].mPacketId, index);
            }
            mItemRegistrySource         = ItemRegistrySource::None;
            mItemRegistryItems          = 0;
            mItemRegistryCustom         = 0;
            mItemRegistryComponentBased = 0;
        }
    }
    {
        std::scoped_lock lock(mPendingPortableChunksMutex);
        mPendingPortableChunks.clear();
    }
}

void Recorder::endTick(bool close) {
    const auto state = mState.load();
    if (state != State::Recording && state != State::Closing) return;

    if (mAsyncReplaySaver) {
        if (auto error = mAsyncReplaySaver->getError()) {
            failRecording("Replay saver failed: " + *error);
            return;
        }
    }

    auto  clientInstance   = ll::service::getClientInstance();
    auto* localPlayer      = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    bool  dimensionChanged = false;
    if (mRecordingDimension) {
        if (!localPlayer) {
            discardLocalViewFrames();
            if (!close && mDimensionTransitionPending.load(std::memory_order_acquire)) {
                if (!flushGamePackets() || !writeTickBoundary()) return;
                return;
            }
            if (close && mHasOpenChunk) {
                if (!flushGamePackets() || !writeTickBoundary() || !finishCurrentChunk(true)) return;
                return;
            }
            failRecording("The local player is unavailable while recording");
            return;
        }
        dimensionChanged = localPlayer->getDimensionId() != *mRecordingDimension;
    }

    // A rotation snapshot belongs to the chunk being finished. An initial snapshot still in slices has to survive a
    // close, or stopping during that first second would throw the recording away.
    if (dimensionChanged || (close && !(mRotationJob && mRotationJob->initial))) discardRotationSnapshot();

    if (dimensionChanged) {
        auto const currentDimensionId = localPlayer->getDimensionId();

        // Finish the client tick in the old chunk before snapshotting the state produced by that tick.
        if (!writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements() || !writeLocalCameraOffset()
            || !writeLocalViewSamples() || !writeTickBoundary() || !finishCurrentChunk(close)) {
            return;
        }
        if (close) return;

        // The new dimension has no columns yet, so defer the snapshot to the retrying initial-snapshot path.
        mRecordingDimension       = currentDimensionId;
        mNeedsInitialSnapshot     = true;
        mPendingDimensionSnapshot = true;
        mDimensionEnteredAt       = std::chrono::steady_clock::now();

        bool const reachedRequestedDimension =
            !mDimensionTransitionPending.load(std::memory_order_acquire)
            || currentDimensionId == mDimensionTransitionTargetId.load(std::memory_order_acquire);
        if (reachedRequestedDimension) {
            mDimensionTransitionPending  = false;
            mDimensionTransitionTargetId = 0;
        }
        return;
    }

    bool const rotationDue =
        !close && !mNeedsInitialSnapshot.load() && mHasOpenChunk && mTicksInCurrentChunk + 1 >= RECORD_CHUNK_TICKS;

    // The terrain is serialized in bounded slices over the next ticks, which keep going into the current chunk.
    bool rotateChunk = false;
    if (rotationDue && !mRotationJob) {
        auto launched = launchRotationSnapshot(ChunkRotationBarrierTimeout);
        if (launched != SnapshotCaptureResult::Success) {
            getLogger().error("Replay snapshot launch failed: {}", mSnapshotFailure);
            failRecording(mSnapshotFailure.empty() ? "Unable to prepare the replay chunk snapshot" : mSnapshotFailure);
            return;
        }
    } else if (rotationDue) {
        bool const drained = runRotationSlice();
        bool const capped  = mRotationJob->slices >= ROTATION_JOB_MAX_SLICES;
        // The slice that finishes the background pass already used this tick's budget. The final pass starts next tick.
        if (!mRotationJob->finalPass && drained && !capped) {
            auto const begun = beginSnapshotFinalPass();
            if (begun == SnapshotCaptureResult::Failed) {
                getLogger().error("Replay snapshot final pass failed: {}", mSnapshotFailure);
                failRecording(
                    mSnapshotFailure.empty() ? "Unable to prepare the replay chunk snapshot" : mSnapshotFailure
                );
                return;
            }
        } else {
            rotateChunk = mRotationJob->finalPass ? (drained || capped) : capped;
        }
    }

    if (rotateChunk) {
        auto                                captureStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration barrierWait{};
        auto                                captureResult = finishRotationSnapshot(barrierWait);
        auto                                elapsed       = std::chrono::steady_clock::now() - captureStart;

        if (captureResult != SnapshotCaptureResult::Success) {
            getLogger().error(
                "Replay snapshot capture failed after {:.3f} ms: {}",
                std::chrono::duration<double, std::milli>(elapsed).count(),
                mSnapshotFailure
            );
            failRecording(mSnapshotFailure.empty() ? "Unable to prepare the replay chunk snapshot" : mSnapshotFailure);
            return;
        }

        if (!writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements() || !writeLocalCameraOffset()
            || !writeLocalViewSamples() || !writeTickBoundary() || !finishCurrentChunk(false) || !writeSnapshot()
            || !commitChunkSnapshot(elapsed, barrierWait)) {
            return;
        }
        return;
    }

    if (!writeInitialSnapshotIfNeeded() || !writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements()
        || !writeLocalCameraOffset() || !writeLocalViewSamples() || !writeTickBoundary()) {
        return;
    }
    if (close && !finishCurrentChunk(true)) return;
}

void Recorder::resetChunkSnapshot() {
    mSnapshotLevelChunks.clear();
    mSnapshotSubChunks.clear();
    mSnapshotConfigurationPackets.clear();
    mSnapshotEntityPackets.clear();
    mSnapshotLocalPlayerPayload.reset();
    mSnapshotView.reset();
    mSnapshotCamera.reset();
    mSnapshotDimension.reset();
    mSnapshotFailure.clear();
}

Recorder::SnapshotCaptureResult Recorder::captureChunkSnapshot(
    std::chrono::steady_clock::duration& barrierWait,
    std::chrono::milliseconds            barrierTimeout
) {
    resetChunkSnapshot();

    SnapshotColumnsJob job;
    auto const         prepared = prepareSnapshotColumns(
        job,
        barrierWait,
        barrierTimeout,
        &mSnapshotColumnCache,
        mSnapshotColumnCacheDimension,
        &mSnapshotCarriedDirtyColumns
    );
    if (prepared != SnapshotCaptureResult::Success) {
        restoreSnapshotDirtyColumns(job);
        return prepared;
    }
    runSnapshotColumns(job);
    auto const finished = finishSnapshotColumns(job);
    if (finished != SnapshotCaptureResult::Success) restoreSnapshotDirtyColumns(job);
    return finished;
}

void Recorder::restoreSnapshotDirtyColumns(SnapshotColumnsJob& job) {
    if (job.dirtyTaken.empty()) return;
    std::scoped_lock lock(mSnapshotDirtyColumnsMutex);
    mSnapshotDirtyColumns.merge(job.dirtyTaken);
    job.dirtyTaken.clear();
}

Recorder::SnapshotCaptureResult Recorder::prepareSnapshotColumns(
    SnapshotColumnsJob&                                           job,
    std::chrono::steady_clock::duration&                          barrierWait,
    std::chrono::milliseconds                                     barrierTimeout,
    std::unordered_map<ChunkPos, SnapshotColumnCacheEntry> const* cache,
    std::optional<DimensionType>                                  cacheDimension,
    std::unordered_set<ChunkPos> const*                           carriedDirty
) {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        mSnapshotFailure = "The local player is not ready for a chunk snapshot";
        return SnapshotCaptureResult::NotReady;
    }

    auto const dimension = localPlayer->getDimensionId();
    auto&      columns   = job.columns;
    Dimension* capturedDimension{};

    // The barrier only has to cover the storage walk; holding shared_ptrs keeps the columns alive for serialization.
    {
        auto mutationGuard = ChunkMutationBarrier::capture(barrierTimeout);
        barrierWait        = mutationGuard.waited();
        if (!mutationGuard) {
            mSnapshotFailure = "Unable to acquire the chunk mutation barrier";
            return SnapshotCaptureResult::NotReady;
        }

        auto* guardedPlayer = clientInstance->getLocalPlayer();
        if (guardedPlayer != localPlayer || !guardedPlayer || guardedPlayer->getDimensionId() != dimension) {
            mSnapshotFailure = "The player or dimension changed while acquiring the chunk mutation barrier";
            return SnapshotCaptureResult::NotReady;
        }

        capturedDimension = &guardedPlayer->getDimension();
        {
            std::scoped_lock lock(mSnapshotDirtyColumnsMutex);
            job.dirtyTaken.swap(mSnapshotDirtyColumns);
        }

        auto const& storage = capturedDimension->getChunkSource().getStorage();
        columns.reserve(storage.size());
        for (auto const& [pos, weakChunk] : storage) {
            auto chunk = weakChunk.lock();
            if (!chunk || chunk->mIsEmptyClientChunk
                || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
                continue;
            }
            columns.push_back(SnapshotColumn{pos, std::move(chunk)});
        }
    }

    // A dimension change arrives before its columns load; retry instead of storing a snapshot with no terrain.
    if (columns.empty()) {
        mSnapshotFailure = "No loaded columns are available for a chunk snapshot yet";
        return SnapshotCaptureResult::NotReady;
    }

    auto& dimensionObject = *capturedDimension;

    auto const& position = localPlayer->getPosition();
    auto const& rotation = localPlayer->getRotation();
    auto const  view     = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    auto const  camera   = captureLocalCameraSample(*localPlayer);
    auto const  center   = SubChunkPos{
        static_cast<int>(std::floor(view.x / 16.0f)),
        static_cast<int>(std::floor(view.y / 16.0f)),
        static_cast<int>(std::floor(view.z / 16.0f))
    };

    std::sort(columns.begin(), columns.end(), [&center](auto const& left, auto const& right) {
        auto leftX         = left.pos.x - center.x;
        auto leftZ         = left.pos.z - center.z;
        auto rightX        = right.pos.x - center.x;
        auto rightZ        = right.pos.z - center.z;
        auto leftDistance  = leftX * leftX + leftZ * leftZ;
        auto rightDistance = rightX * rightX + rightZ * rightZ;
        if (leftDistance != rightDistance) return leftDistance < rightDistance;
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        return left.pos.z < right.pos.z;
    });

    auto air = Block::tryGetFromRegistry(BedrockBlockNames::Air());
    if (!air) {
        mSnapshotFailure = "Unable to resolve the engine air block for a chunk snapshot";
        return SnapshotCaptureResult::Failed;
    }

    auto const dimensionMinHeight = static_cast<int>(dimensionObject.mHeightRange->mMin);
    auto const dimensionMaxHeight = static_cast<int>(dimensionObject.mHeightRange->mMax);
    auto const expectedSubChunks  = static_cast<size_t>(dimensionObject.getHeightInSubchunks());
    if (dimensionMinHeight % 16 != 0 || dimensionMaxHeight <= dimensionMinHeight
        || static_cast<size_t>((dimensionMaxHeight - dimensionMinHeight) / 16) != expectedSubChunks) {
        mSnapshotFailure = "The current dimension has an invalid subchunk height range";
        return SnapshotCaptureResult::Failed;
    }

    job.player        = localPlayer;
    job.view          = view;
    job.camera        = camera;
    job.dimension     = dimension;
    job.minHeight     = dimensionMinHeight;
    job.maxHeight     = dimensionMaxHeight;
    job.dimensionName = *dimensionObject.mName;
    job.air           = &*air;
    if (cache && cacheDimension == dimension) {
        for (auto& column : columns) {
            auto const cached = cache->find(column.pos);
            if (cached == cache->end() || cached->second.chunk.lock() != column.chunk) continue;
            column.cached = cached->second;
            column.packetDirty =
                job.dirtyTaken.contains(column.pos) || (carriedDirty && carriedDirty->contains(column.pos));
        }
    }
    job.results.resize(columns.size());
    return SnapshotCaptureResult::Success;
}

void Recorder::serializeSnapshotColumns(SnapshotColumnsJob& job) {
    auto const&  columns            = job.columns;
    size_t const numColumns         = columns.size();
    auto const   dimension          = job.dimension;
    auto const   dimensionMinHeight = job.minHeight;
    auto const*  air                = job.air;
    bool const   finalPass          = job.finalPass;

    auto saveContext = SaveContextFactory::createNetworkSaveContext();
    if (!saveContext) {
        auto const ci = job.next.fetch_add(1, std::memory_order_relaxed);
        if (ci < numColumns) job.results[ci].error = "Unable to create a network SaveContext for block actors";
        return;
    }

    // Column cost varies a lot with terrain, so workers claim columns one at a time instead of fixed ranges.
    while (true) {
        auto const deadline = job.sliceDeadlineNs.load(std::memory_order_relaxed);
        if (deadline != 0 && steadyNowNs() >= deadline) return;
        size_t const ci = job.next.fetch_add(1, std::memory_order_relaxed);
        if (ci >= numColumns) return;

        auto&       result = job.results[ci];
        auto const& column = columns[ci];
        auto const& pos    = column.pos;
        auto&       chunk  = *column.chunk;
        // True when the worker should stop; a sliced job records the error and moves on.
        auto const fail = [&result, &job](std::string error) {
            result.error = std::move(error);
            return job.stopOnError;
        };

        if (chunk.mIsEmptyClientChunk || chunk.mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
            if (fail(snapshotFailure(pos, std::nullopt, "starting column serialization", "chunk unloaded"))) return;
            continue;
        }

        auto const& subChunks = *chunk.mSubChunks;
        if (subChunks.empty()) {
            if (fail(snapshotFailure(pos, std::nullopt, "validating slots", "no subchunk slots"))) return;
            continue;
        }

        {
            // Read before serializing, so a change that lands during serialization shows up next time.
            result.subChunkFingerprint = fingerprintSubChunks(chunk);
            result.chunkFingerprint    = fingerprintChunk(chunk);
            auto const* cached         = column.cached ? &*column.cached : nullptr;
            enum class Reason { Missing, Packet, SubChunks, Chunk, Reuse } reason = Reason::Reuse;
            if (!cached) {
                reason = Reason::Missing;
            } else if (column.packetDirty) {
                reason = Reason::Packet;
            } else if (cached->subChunkFingerprint != result.subChunkFingerprint) {
                reason = Reason::SubChunks;
            } else if (cached->chunkFingerprint != result.chunkFingerprint) {
                reason = Reason::Chunk;
            }
            if (reason == Reason::Reuse) {

                result.levelChunk = cached->levelChunk;
                result.subChunk   = cached->subChunk;
                result.done       = true;
                continue;
            }

            std::string stage = "creating LevelChunkPacket";
            try {
                auto levelBase = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
                if (!levelBase || levelBase->getId() != MinecraftPacketIds::FullChunkData) {
                    if (fail(snapshotFailure(pos, std::nullopt, stage, "native packet factory returned wrong type"))) {
                        return;
                    }
                    continue;
                }
                auto level = std::static_pointer_cast<LevelChunkPacket>(std::move(levelBase));

                level->mPos                           = pos;
                level->mDimensionId                   = dimension;
                level->mCacheEnabled                  = false;
                level->mSubChunksCount                = 0;
                level->mClientNeedsToRequestSubchunks = true;
                level->mClientRequestSubChunkLimit    = -1;
                level->mCacheMetadata->clear();

                stage = "serializing biome and border data";
                BinaryStream     levelPayload;
                VarIntDataOutput levelOutput(levelPayload);
                chunk.serializeBiomes(levelOutput);
                chunk.serializeBorderBlocks(levelOutput);
                level->mSerializedChunk = std::move(levelPayload.mBuffer);

                stage             = "creating SubChunkPacket";
                auto subChunkBase = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
                if (!subChunkBase || subChunkBase->getId() != MinecraftPacketIds::SubChunkPacket) {
                    if (fail(snapshotFailure(pos, std::nullopt, stage, "native packet factory returned wrong type"))) {
                        return;
                    }
                    continue;
                }
                auto      subChunkPacket   = std::static_pointer_cast<SubChunkPacket>(std::move(subChunkBase));
                int const minimumSubChunkY = dimensionMinHeight / 16;

                subChunkPacket->mCacheEnabled  = false;
                subChunkPacket->mDimensionType = dimension;
                subChunkPacket->mCenterPos     = SubChunkPos{pos.x, minimumSubChunkY, pos.z};
                subChunkPacket->mSubChunkData->clear();
                subChunkPacket->mSubChunkData->reserve(subChunks.size());

                for (size_t index = 0; index < subChunks.size(); ++index) {
                    auto const& subChunk        = subChunks[index];
                    int const   actualAbsoluteY = static_cast<int>(static_cast<schar>(subChunk.mAbsoluteIndex));
                    stage                       = "validating subchunk slot";

                    if (subChunk.isPlaceHolderSubChunk()) continue;
                    if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
                        && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished
                        && subChunk.mSubChunkState != SubChunk::SubChunkState::ProcessedSubChunk) {
                        continue;
                    }

                    int const relativeY = actualAbsoluteY - minimumSubChunkY;
                    if (relativeY < std::numeric_limits<schar>::min()
                        || relativeY > std::numeric_limits<schar>::max()) {
                        result.error =
                            snapshotFailure(pos, actualAbsoluteY, stage, "subchunk offset is outside the packet range");
                        break;
                    }

                    BinaryStream serializedSubChunk;
                    bool const   allAir = subChunk.isUniform(*air);

                    if (!allAir) {
                        stage = "serializing subchunk";
                        VarIntDataOutput subChunkOutput(serializedSubChunk);
                        subChunk.serialize(subChunkOutput, false);
                    }

                    stage = "serializing block actors";
                    {
                        VarIntDataOutput blockActorOutput(serializedSubChunk);
                        chunk.serializeBlockEntitiesForSubChunk(
                            blockActorOutput,
                            SubChunkPos{pos.x, actualAbsoluteY, pos.z},
                            *saveContext
                        );
                    }

                    SubChunkPacket::SubChunkPosOffset offset{};
                    offset.mX             = 0;
                    offset.mY             = static_cast<schar>(relativeY);
                    offset.mZ             = 0;
                    auto const resultFlag = allAir ? SubChunkPacket::SubChunkRequestResult::SuccessAllAir
                                                   : SubChunkPacket::SubChunkRequestResult::Success;
                    subChunkPacket->mSubChunkData->emplace_back(offset, resultFlag);
                    auto& data               = subChunkPacket->mSubChunkData->back();
                    data.mSerializedSubChunk = std::move(serializedSubChunk.mBuffer);
                    data.mBlobId             = 0;

                    stage = "populating heightmaps";
                    chunk.populateHeightMapDataForSubChunkPacket(static_cast<short>(actualAbsoluteY), data);
                }

                if (!result.error.empty()) {
                    if (job.stopOnError) return;
                    continue;
                }
                result.levelChunk = std::move(level);
                if (!subChunkPacket->mSubChunkData->empty()) {
                    result.subChunk = std::move(subChunkPacket);
                }
            } catch (std::exception const& exception) {
                if (fail(snapshotFailure(pos, std::nullopt, stage, exception.what()))) return;
                continue;
            } catch (...) {
                if (fail(snapshotFailure(pos, std::nullopt, stage, "unknown engine serialization error"))) return;
                continue;
            }
        }
        result.done = true;
    }
}

void Recorder::runSnapshotColumns(SnapshotColumnsJob& job) {
    // Pool threads stay alive: creating and exiting threads per snapshot stalled the client thread for seconds.
    runParallel(job.columns.empty() ? 0 : job.columns.size() - 1, [&job] { serializeSnapshotColumns(job); });
}

Recorder::SnapshotCaptureResult Recorder::launchRotationSnapshot(std::chrono::milliseconds barrierTimeout) {
    auto const                          start = std::chrono::steady_clock::now();
    auto                                job   = std::make_unique<SnapshotColumnsJob>();
    std::chrono::steady_clock::duration barrierWait{};
    job->stopOnError    = false;
    auto const prepared = prepareSnapshotColumns(
        *job,
        barrierWait,
        barrierTimeout,
        &mSnapshotColumnCache,
        mSnapshotColumnCacheDimension,
        &mSnapshotCarriedDirtyColumns
    );
    if (prepared != SnapshotCaptureResult::Success) {
        restoreSnapshotDirtyColumns(*job);
        return prepared;
    }

    // Half the cores, so the slices leave room for the engine's own workers.
    size_t const threads =
        std::min(job->columns.size(), static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency() / 2)));
    job->sliceHelpers = threads == 0 ? 0 : threads - 1;
    job->launchCostNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()
    );
    job->barrierWait = barrierWait;
    mRotationJob     = std::move(job);
    return SnapshotCaptureResult::Success;
}

bool Recorder::runRotationSlice() {
    auto&      job   = *mRotationJob;
    auto const start = steadyNowNs();
    job.sliceDeadlineNs.store(start + RotationSliceBudgetNs, std::memory_order_relaxed);
    // The client thread is blocked for the whole slice, so chunk applies and block actor lists stay put. Subchunk
    // insert workers can still swap block storage; the old storage is freed late enough for a read in progress, and
    // the fingerprints send that column through the final pass again.
    runParallel(job.sliceHelpers, [&job] { serializeSnapshotColumns(job); });
    auto const elapsed = steadyNowNs() - start;
    ++job.slices;
    job.sliceTotalNs += elapsed;
    job.sliceMaxNs    = std::max(job.sliceMaxNs, elapsed);
    return job.next.load(std::memory_order_relaxed) >= job.columns.size();
}

Recorder::SnapshotCaptureResult Recorder::beginSnapshotFinalPass() {
    auto launched = std::move(mRotationJob);

    // Background results are the cache. A column is redone only when its fingerprint moved or a newer dirty mark
    // arrived during the slices. The launch dirty set is not forced through again: those bytes were just written,
    // and a mark that still has not landed stays carried for the next snapshot.
    std::unordered_map<ChunkPos, SnapshotColumnCacheEntry> interim;
    interim.reserve(launched->columns.size());
    for (size_t index = 0; index < launched->columns.size(); ++index) {
        auto const& result = launched->results[index];
        if (!result.done || !result.error.empty()) continue;
        auto const& column = launched->columns[index];
        interim.insert_or_assign(
            column.pos,
            SnapshotColumnCacheEntry{
                column.chunk,
                result.subChunkFingerprint,
                result.chunkFingerprint,
                result.levelChunk,
                result.subChunk
            }
        );
    }

    auto job                    = std::make_unique<SnapshotColumnsJob>();
    job->finalPass              = true;
    job->initial                = launched->initial;
    job->stopOnError            = false;
    job->backgroundSlices       = launched->slices;
    job->backgroundSliceTotalNs = launched->sliceTotalNs;
    job->backgroundSliceMaxNs   = launched->sliceMaxNs;
    job->backgroundLaunchCostNs = launched->launchCostNs;
    job->backgroundColumns      = launched->columns.size();
    job->backgroundDirty        = std::move(launched->dirtyTaken);

    auto const timeout = job->initial ? InitialSnapshotBarrierTimeout : ChunkRotationBarrierTimeout;
    auto const result = prepareSnapshotColumns(*job, job->barrierWait, timeout, &interim, launched->dimension, nullptr);
    if (result != SnapshotCaptureResult::Success) {
        restoreSnapshotDirtyColumns(*job);
        launched->dirtyTaken = std::move(job->backgroundDirty);
        mRotationJob         = std::move(launched);
        return result;
    }

    size_t const threads =
        std::min(job->columns.size(), static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency() / 2)));
    job->sliceHelpers = threads == 0 ? 0 : threads - 1;
    mRotationJob      = std::move(job);
    return SnapshotCaptureResult::Success;
}

Recorder::SnapshotCaptureResult Recorder::finishRotationSnapshot(std::chrono::steady_clock::duration& barrierWait) {
    // The slice cap can expire before the background pass drains. One unbounded join then closes the chunk.
    if (!mRotationJob->finalPass) {
        auto const begun = beginSnapshotFinalPass();
        if (begun != SnapshotCaptureResult::Success) return begun;
    }

    auto& job = *mRotationJob;
    if (job.next.load(std::memory_order_relaxed) < job.columns.size()) {
        job.sliceDeadlineNs.store(0, std::memory_order_relaxed);
        runSnapshotColumns(job);
    }

    barrierWait = job.barrierWait;
    auto result = finishSnapshotColumns(job);
    if (result != SnapshotCaptureResult::Success) {
        restoreSnapshotDirtyColumns(job);
        discardRotationSnapshot();
        return result;
    }

    auto const stallNs = std::max(job.backgroundSliceMaxNs, job.sliceMaxNs);
    if (stallNs > 0) {
        mLongestSnapshotStall = std::max(
            mLongestSnapshotStall,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(stallNs))
        );
    }
    mSnapshotCarriedDirtyColumns.merge(job.backgroundDirty);

    job.backgroundDirty.clear();
    job.dirtyTaken.clear();
    mRotationJob.reset();
    return SnapshotCaptureResult::Success;
}
void Recorder::discardRotationSnapshot() {
    if (!mRotationJob) return;
    restoreSnapshotDirtyColumns(*mRotationJob);
    if (!mRotationJob->backgroundDirty.empty()) {
        std::scoped_lock lock(mSnapshotDirtyColumnsMutex);
        mSnapshotDirtyColumns.merge(mRotationJob->backgroundDirty);
        mRotationJob->backgroundDirty.clear();
    }
    mRotationJob.reset();
}

Recorder::SnapshotCaptureResult Recorder::finishSnapshotColumns(SnapshotColumnsJob& job) {
    auto const&  columns    = job.columns;
    size_t const numColumns = columns.size();
    // Claims are in index order, so the first error is the lowest failing column, like a serial capture.
    for (size_t index = 0; index < numColumns; ++index) {
        auto& result = job.results[index];
        if (!result.error.empty()) {
            mSnapshotFailure = std::move(result.error);
            return SnapshotCaptureResult::Failed;
        }
        if (!result.done) {
            mSnapshotFailure =
                snapshotFailure(columns[index].pos, std::nullopt, "serializing the column", "no worker finished it");
            return SnapshotCaptureResult::Failed;
        }
    }

    // Distance order, so the snapshot layout does not depend on worker scheduling.
    std::vector<std::shared_ptr<LevelChunkPacket>>         levelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>           subChunkPackets;
    std::unordered_map<ChunkPos, SnapshotColumnCacheEntry> nextColumnCache;
    levelChunks.reserve(numColumns);
    subChunkPackets.reserve(numColumns);
    nextColumnCache.reserve(numColumns);
    for (size_t index = 0; index < numColumns; ++index) {
        auto& result = job.results[index];
        nextColumnCache.insert_or_assign(
            columns[index].pos,
            SnapshotColumnCacheEntry{
                columns[index].chunk,
                result.subChunkFingerprint,
                result.chunkFingerprint,
                result.levelChunk,
                result.subChunk
            }
        );
        levelChunks.emplace_back(std::move(result.levelChunk));
        if (result.subChunk) subChunkPackets.emplace_back(std::move(result.subChunk));
    }

    auto        clientInstance = ll::service::getClientInstance();
    auto* const localPlayer    = job.player;
    auto const  dimension      = job.dimension;
    auto*       finalPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (finalPlayer != localPlayer || !finalPlayer || finalPlayer->getDimensionId() != dimension) {
        mSnapshotFailure = "The player or dimension changed while capturing the chunk snapshot";
        return SnapshotCaptureResult::NotReady;
    }

    std::vector<PlaybackSerializedGamePacket> entityPackets;
    auto                                      appendEntityPacket = [&entityPackets](Packet const& packet) {
        PlaybackBuffer stream;
        packet.write(stream);
        entityPackets.push_back({static_cast<int32_t>(packet.getId()), std::move(stream.mBuffer)});
    };

    try {
        auto& level = finalPlayer->getLevel();
        if (!mRecordedLocalPlayerId) {
            mRecordedLocalPlayerId        = RecordedPlayerUniqueId;
            mRecordedLocalPlayerRuntimeId = RecordedPlayerRuntimeId;
            mRecordedLocalPlayerUuid      = mce::UUID::random();
        }

        auto timePacket = MinecraftPackets::createPacket(MinecraftPacketIds::SetTime);
        if (!timePacket) {
            mSnapshotFailure = "Unable to create the snapshot time packet";
            return SnapshotCaptureResult::Failed;
        }
        static_cast<SetTimePacket&>(*timePacket).mTime = level.getTime();
        appendEntityPacket(*timePacket);

        auto spawnPacket = MinecraftPackets::createPacket(MinecraftPacketIds::SetSpawnPosition);
        if (!spawnPacket) {
            mSnapshotFailure = "Unable to create the snapshot spawn position packet";
            return SnapshotCaptureResult::Failed;
        }
        auto const& spawnPosition = level.getSharedSpawnPos();
        auto&       spawn         = static_cast<SetSpawnPositionPacket&>(*spawnPacket);
        spawn.mPos                = spawnPosition;
        spawn.mSpawnPosType       = SpawnPositionType::WorldSpawn;
        spawn.mDimensionType      = dimension;
        spawn.mSpawnBlockPos      = spawnPosition;
        appendEntityPacket(*spawnPacket);

        auto appendWeatherEvent = [&appendEntityPacket](SharedTypes::Legacy::LevelEvent event) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::LevelEvent);
            if (!packet) return false;
            auto& weather    = static_cast<LevelEventPacket&>(*packet);
            weather.mEventId = static_cast<int>(event);
            weather.mPos     = Vec3{};
            weather.mData    = 0;
            appendEntityPacket(*packet);
            return true;
        };
        auto const& levelData = level.getLevelData();
        if (!appendWeatherEvent(
                levelData.mRainLevel > 0.0f ? SharedTypes::Legacy::LevelEvent::StartRaining
                                            : SharedTypes::Legacy::LevelEvent::StopRaining
            )
            || !appendWeatherEvent(
                levelData.mLightningLevel > 0.0f ? SharedTypes::Legacy::LevelEvent::StartThunderstorm
                                                 : SharedTypes::Legacy::LevelEvent::StopThunderstorm
            )) {
            mSnapshotFailure = "Unable to create snapshot weather packets";
            return SnapshotCaptureResult::Failed;
        }

        ensureRecordedItemRegistry(level);

        auto actors = level.getRuntimeActorList();

        // Identity (the menu) is everyone the client already lists, plus any spawned player the list missed.
        // Spawn packets are only for living players in this dimension. One write point, no second pass.
        std::vector<Player*>        spawnPlayers;
        std::unordered_set<int64_t> spawnIds;
        std::unordered_set<int64_t> deadIds;
        int                         runtimePlayers = 0;
        auto const                  considerSpawn  = [&](Player& player, bool fromRuntimeList) {
            if (&player == finalPlayer || player.getDimensionId() != dimension) return;
            auto const uniqueId = player.getOrCreateUniqueID().rawID;
            if (!player.isAlive()) {
                deadIds.insert(uniqueId);
                return;
            }
            if (!spawnIds.insert(uniqueId).second) return;
            spawnPlayers.push_back(&player);
            if (fromRuntimeList) ++runtimePlayers;
        };
        for (auto* actor : actors) {
            if (!actor || !actor->isPlayer()) continue;
            considerSpawn(static_cast<Player&>(*actor), true);
        }
        level.forEachPlayer([&](Player& player) {
            considerSpawn(player, false);
            return true;
        });

        auto const                              playerUniqueId = finalPlayer->getOrCreateUniqueID();
        auto const                              playerUuid     = finalPlayer->getUuid().asString();
        std::vector<PlayerListEntry>            identity;
        std::unordered_map<std::string, size_t> identityIndex;
        auto const hasSkin = [](PlayerListEntry const& entry) { return static_cast<bool>(entry.mSkin->mSkinImpl); };
        std::unordered_map<std::string, Player const*> playersByUuid;
        playersByUuid.emplace(playerUuid, finalPlayer);
        level.forEachPlayer([&](Player& player) {
            playersByUuid.emplace(player.getUuid().asString(), &player);
            return true;
        });
        auto const upsertIdentity = [&](PlayerListEntry entry) {
            // A listed entry can lack its skin; fall back to the skin the player actor already renders with.
            if (!hasSkin(entry)) {
                auto const source = playersByUuid.find(entry.mUUID->asString());
                if (source != playersByUuid.end()) {
                    if (auto const& ownSkin = source->second->mSkin; ownSkin && static_cast<bool>(ownSkin->mSkinImpl)) {
                        entry.mSkin->mSkinImpl = ownSkin->mSkinImpl;
                    }
                }
            }
            auto const entryId = entry.mId->rawID;
            if (entry.mUUID->asString() == playerUuid || (entryId != 0 && entryId == playerUniqueId.rawID)) {
                entry.mId   = *mRecordedLocalPlayerId;
                entry.mUUID = *mRecordedLocalPlayerUuid;
                entry.mXUID->clear();
                entry.mPlatformOnlineId->clear();
            }
            auto const key = entry.mUUID->asString();
            if (key.empty()) return;
            auto const found = identityIndex.find(key);
            if (found == identityIndex.end()) {
                identityIndex.emplace(key, identity.size());
                identity.push_back(std::move(entry));
                return;
            }
            auto& existing = identity[found->second];
            if (!hasSkin(existing) && hasSkin(entry)) existing = std::move(entry);
        };
        for (auto const& listed : level.getPlayerList()) upsertIdentity(PlayerListEntry(listed.second));
        upsertIdentity(PlayerListEntry(*finalPlayer));
        for (auto const* player : spawnPlayers) upsertIdentity(PlayerListEntry(*player));

        if (!identity.empty()) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerList);
            if (!packet) {
                mSnapshotFailure = "Unable to create the entity snapshot player list packet";
                return SnapshotCaptureResult::Failed;
            }

            auto& playerListPacket   = static_cast<PlayerListPacket&>(*packet);
            playerListPacket.mAction = PlayerListPacketType::Add;
            playerListPacket.mEntries->reserve(identity.size());
            for (auto& entry : identity) playerListPacket.mEntries->emplace_back(std::move(entry));
            appendEntityPacket(playerListPacket);
        }

        auto appendActorState = [&](Actor const& actor, ActorRuntimeID runtimeId) {
            for (auto const& effect : actor.getAllEffects()) {
                auto effectPacket = MinecraftPackets::createPacket(MinecraftPacketIds::MobEffect);
                if (!effectPacket) {
                    mSnapshotFailure = "Unable to create an entity snapshot effect packet";
                    return false;
                }
                auto& payload                = static_cast<MobEffectPacket&>(*effectPacket);
                payload.mRuntimeId           = runtimeId;
                payload.mEffectDurationTicks = *effect.mDuration;
                payload.mEventId             = MobEffectPacketPayload::Event::Add;
                payload.mEffectId            = static_cast<int>(effect.mId);
                payload.mEffectAmplifier     = effect.mAmplifier;
                payload.mShowParticles       = effect.mEffectVisible;
                payload.mAmbient             = effect.mAmbient;
                payload.mTick                = PlayerInputTick{};
                appendEntityPacket(*effectPacket);
            }

            if (actor.isPlayer() || actor.hasCategory(ActorCategory::Mob)) {
                auto const selectedSlot =
                    actor.isPlayer() ? static_cast<Player const&>(actor).getSelectedItemSlot() : 0;
                PlaybackSetEquipmentPacket equipment(actor, runtimeId, selectedSlot);
                for (auto const& packet : equipment.createPackets()) {
                    appendEntityPacket(*packet);
                }
            }
            return true;
        };

        // The recorded local player is required snapshot state, independent of the transient runtime actor list.
        AddPlayerPacket recordedPlayer(*finalPlayer);
        remapRecordedPlayerReferences(
            recordedPlayer,
            finalPlayer->getOrCreateUniqueID(),
            finalPlayer->getRuntimeID(),
            finalPlayer->getUuid(),
            *mRecordedLocalPlayerId,
            *mRecordedLocalPlayerRuntimeId,
            *mRecordedLocalPlayerUuid
        );
        recordedPlayer.mPlayerGameType = finalPlayer->getPlayerGameType();
        PlaybackBuffer recordedPlayerStream;
        recordedPlayer.write(recordedPlayerStream);
        mSnapshotLocalPlayerPayload = std::move(recordedPlayerStream.mBuffer);
        if (!appendActorState(*finalPlayer, *mRecordedLocalPlayerRuntimeId)) {
            return SnapshotCaptureResult::Failed;
        }

        // The observer's HUD state: a seek restores it from the snapshot instead of waiting for the next change.
        for (int slot = 0; slot < HudHotbarSlots; ++slot) {
            appendEntityPacket(*makeHudSlotPacket(
                ContainerID::Inventory,
                slot,
                finalPlayer->getSupplies().getItem(slot, ContainerID::Inventory)
            ));
        }
        for (int slot = 0; slot < HudArmorSlots; ++slot) {
            appendEntityPacket(*makeHudSlotPacket(
                ContainerID::Armor,
                slot,
                finalPlayer->getArmor(static_cast<SharedTypes::Legacy::ArmorSlot>(slot))
            ));
        }
        appendEntityPacket(*makeHudSlotPacket(ContainerID::Offhand, 0, finalPlayer->getOffhandSlot()));
        appendEntityPacket(*makeHudSelectedSlotPacket(*finalPlayer));
        appendEntityPacket(*makeHudAttributePacket(*finalPlayer, *mRecordedLocalPlayerRuntimeId));
        appendEntityPacket(*makeHudGameTypePacket(*finalPlayer, *mRecordedLocalPlayerId));
        for (auto const* item : {&finalPlayer->getCarriedItem(), &finalPlayer->getOffhandSlot()}) {
            auto const mapId = heldMapId(*item);
            if (!mapId) continue;
            if (auto mapPacket = makeHeldMapPacket(*finalPlayer, *mapId)) appendEntityPacket(*mapPacket);
        }

        std::unordered_set<int64_t> serializedPlayers;
        for (auto* actor : actors) {
            if (!actor || actor == finalPlayer || !actor->isAlive() || actor->getDimensionId() != dimension) continue;
            if (actor->isPlayer()) continue;

            auto packet = actor->tryCreateAddActorPacket();
            if (!packet) continue;
            appendEntityPacket(*packet);
            if (!appendActorState(*actor, actor->getRuntimeID())) return SnapshotCaptureResult::Failed;
        }
        for (auto* player : spawnPlayers) {
            AddPlayerPacket packet(*player);
            serializedPlayers.insert(player->getOrCreateUniqueID().rawID);
            appendEntityPacket(packet);
            if (!appendActorState(*player, player->getRuntimeID())) return SnapshotCaptureResult::Failed;
        }
        getLogger().debug(
            "replay snapshot players runtime={} listed={} serialized={} deadSkipped={} identity={}",
            runtimePlayers,
            static_cast<int>(spawnPlayers.size()),
            static_cast<int>(serializedPlayers.size()),
            static_cast<int>(deadIds.size()),
            static_cast<int>(identity.size())
        );

        auto&                                scoreboard = level.getScoreboard();
        std::unordered_set<Objective const*> displayedObjectives;
        for (auto const& slot : scoreboard.getDisplayObjectiveSlotNames()) {
            auto const* display = scoreboard.getDisplayObjective(slot);
            if (!display || !display->mObjective) continue;

            auto const& objective = *display->mObjective;
            auto        packet    = MinecraftPackets::createPacket(MinecraftPacketIds::SetDisplayObjective);
            if (!packet) {
                mSnapshotFailure = "Unable to create a snapshot display objective packet";
                return SnapshotCaptureResult::Failed;
            }
            auto& payload                 = static_cast<SetDisplayObjectivePacket&>(*packet);
            payload.mDisplaySlotName      = slot;
            payload.mObjectiveName        = *objective.mName;
            payload.mObjectiveDisplayName = *objective.mDisplayName;
            payload.mCriteriaName         = objective.mCriteria.mName;
            payload.mSortOrder            = display->mSortOrder;
            appendEntityPacket(*packet);
            displayedObjectives.emplace(&objective);
        }

        auto const localPlayerId = finalPlayer->getOrCreateUniqueID();
        for (auto const* objective : displayedObjectives) {
            for (auto const& [scoreboardId, score] : *objective->mScores) {
                SetScorePacket packet(ScorePacketType::Change, scoreboardId, *objective);
                for (auto& info : *packet.mScoreInfo) {
                    info.mScoreValue = score;
                    if (info.mPlayerId->mActorUniqueId == localPlayerId.rawID) {
                        info.mPlayerId->mActorUniqueId = mRecordedLocalPlayerId->rawID;
                    }
                    if (info.mEntityId == localPlayerId) info.mEntityId = *mRecordedLocalPlayerId;
                }
                appendEntityPacket(packet);
            }
        }
    } catch (std::exception const& exception) {
        mSnapshotFailure = "Unable to serialize the entity snapshot: " + std::string(exception.what());
        return SnapshotCaptureResult::Failed;
    } catch (...) {
        mSnapshotFailure = "Unable to serialize the entity snapshot";
        return SnapshotCaptureResult::Failed;
    }

    mSnapshotLevelChunks   = std::move(levelChunks);
    mSnapshotSubChunks     = std::move(subChunkPackets);
    mSnapshotEntityPackets = std::move(entityPackets);
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mSnapshotConfigurationPackets = mConfigurationPackets;
    }
    mSnapshotView          = job.view;
    mSnapshotCamera        = job.camera;
    mSnapshotDimension     = SnapshotDimension{dimension, job.minHeight, job.maxHeight, job.dimensionName};
    mRecordingDimension    = dimension;

    mSnapshotColumnCache          = std::move(nextColumnCache);
    mSnapshotColumnCacheDimension = dimension;
    mSnapshotCarriedDirtyColumns  = std::move(job.dirtyTaken);
    job.dirtyTaken.clear();

    return SnapshotCaptureResult::Success;
}

bool Recorder::commitChunkSnapshot(
    std::chrono::steady_clock::duration captureElapsed,
    std::chrono::steady_clock::duration barrierWait
) {
    mSnapshotView.reset();
    mSnapshotCamera.reset();
    mSnapshotDimension.reset();
    mLongestSnapshotStall = std::max(mLongestSnapshotStall, captureElapsed);
    mNeedsInitialSnapshot = false;
    mHasOpenChunk         = true;
    mOpenChunkHasData     = true;

    auto elapsedMs = std::chrono::duration<double, std::milli>(captureElapsed).count();
    auto longestMs = std::chrono::duration<double, std::milli>(mLongestSnapshotStall).count();
    auto waitMs    = std::chrono::duration<double, std::milli>(barrierWait).count();
    getLogger().debug(
        "Captured replay snapshot with {} columns in {:.3f} ms (longest stall {:.3f} ms, barrier wait {:.3f} ms)",
        mSnapshotLevelChunks.size(),
        elapsedMs,
        longestMs,
        waitMs
    );
    return true;
}

bool Recorder::writeInitialSnapshotIfNeeded() {
    if (!mNeedsInitialSnapshot.load()) return true;

    // Same 8 ms slices as chunk rotation. Ticks are not written until the snapshot commits, so the baseline matches
    // the world the following tick records. Closing drains whatever is left in this tick.
    bool const finishNow = mState.load() == State::Closing;
    if (!mRotationJob) {
        // The terrain of a new dimension arrives behind the game's loading screen. The snapshot waits for that screen
        // to go, so it holds the terrain the player first saw and the replay has no ticks of the loading itself.
        if (mPendingDimensionSnapshot && !finishNow && isDimensionLoading()) return false;
        auto const launched = launchRotationSnapshot(InitialSnapshotBarrierTimeout);
        if (launched == SnapshotCaptureResult::NotReady) return false;
        if (launched == SnapshotCaptureResult::Failed) {
            getLogger().error("Initial snapshot launch failed: {}", mSnapshotFailure);
            failRecording(
                mSnapshotFailure.empty() ? "Unable to prepare the initial replay chunk snapshot" : mSnapshotFailure
            );
            return false;
        }
        mRotationJob->initial = true;
        if (mPendingDimensionSnapshot) dropPendingSnapshotColumnPackets(*mRotationJob);
        if (!finishNow) return false;
    }

    auto drainNow = [this](SnapshotColumnsJob& job) {
        if (job.next.load(std::memory_order_relaxed) >= job.columns.size()) return;
        job.sliceDeadlineNs.store(0, std::memory_order_relaxed);
        runSnapshotColumns(job);
    };
    if (finishNow && !mRotationJob->finalPass) drainNow(*mRotationJob);

    bool const drained = mRotationJob->finalPass && finishNow ? true : runRotationSlice();
    bool const capped  = mRotationJob->slices >= ROTATION_JOB_MAX_SLICES;
    if (!mRotationJob->finalPass && (drained || finishNow) && !capped) {
        auto const begun = beginSnapshotFinalPass();
        if (begun == SnapshotCaptureResult::NotReady) return false;
        if (begun == SnapshotCaptureResult::Failed) {
            getLogger().error("Initial snapshot final pass failed: {}", mSnapshotFailure);
            failRecording(
                mSnapshotFailure.empty() ? "Unable to prepare the initial replay chunk snapshot" : mSnapshotFailure
            );
            return false;
        }
        // Columns that loaded during the slices are in the final pass as well.
        if (mPendingDimensionSnapshot) dropPendingSnapshotColumnPackets(*mRotationJob);
        if (!finishNow) return false;
    }
    if (finishNow && mRotationJob->finalPass) drainNow(*mRotationJob);
    if (!finishNow && !drained && !capped) return false;

    auto                                captureStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration barrierWait{};
    auto const                          captureResult = finishRotationSnapshot(barrierWait);
    auto const                          elapsed       = std::chrono::steady_clock::now() - captureStart;

    if (captureResult == SnapshotCaptureResult::NotReady) return false;
    if (captureResult == SnapshotCaptureResult::Failed) {
        getLogger().error(
            "Initial snapshot capture failed after {:.3f} ms: {}",
            std::chrono::duration<double, std::milli>(elapsed).count(),
            mSnapshotFailure
        );
        failRecording(
            mSnapshotFailure.empty() ? "Unable to prepare the initial replay chunk snapshot" : mSnapshotFailure
        );
        return false;
    }

    if (mPendingDimensionSnapshot) {
        mCurrentChunkForcePlaySnapshot = true;
        mPendingDimensionSnapshot      = false;
    }

    // The packets queued while the snapshot was being taken are written after it.
    dropPendingSpawnPackets();
    if (!writeSnapshot()) return false;
    return commitChunkSnapshot(elapsed, barrierWait);
}

bool Recorder::isDimensionLoading() const {
    if (std::chrono::steady_clock::now() - mDimensionEnteredAt > DimensionLoadingTimeout) return false;
    auto client = ll::service::getClientInstance();
    return client
        && (client->isShowingLoadingScreen() || client->isShowingProgressScreen()
            || client->isShowingWorldProgressScreen());
}

void Recorder::dropPendingSnapshotColumnPackets(SnapshotColumnsJob const& job) {
    std::unordered_set<ChunkPos> columns;
    columns.reserve(job.columns.size());
    for (auto const& column : job.columns) columns.insert(column.pos);

    // Played after the snapshot, a column packet would load again a column the snapshot has just loaded. Subchunk
    // packets stay: one that had not landed in its column when the column was serialized is only in the timeline.
    std::scoped_lock                          lock(mPendingGamePacketsMutex);
    std::vector<PlaybackSerializedGamePacket> keptPackets;
    std::vector<DeferredCompletedChunk>       keptDeferred;
    keptPackets.reserve(mPendingGamePackets.size());
    size_t deferredIndex = 0;
    for (auto& pending : mPendingGamePackets) {
        bool covered = false;
        if (pending.mPacketId == DeferredChunkPacketId) {
            if (deferredIndex < mDeferredCompletedChunks.size()) {
                auto& deferred = mDeferredCompletedChunks[deferredIndex];
                covered        = deferred.dimensionId == job.dimension && columns.contains(deferred.pos);
                if (!covered) keptDeferred.push_back(std::move(deferred));
            }
            ++deferredIndex;
        } else if (pending.mPacketId == static_cast<int32_t>(MinecraftPacketIds::FullChunkData)) {
            auto                 packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
            ReadOnlyBinaryStream input(pending.mPayload, false);
            if (packet && packet->read(input)) {
                auto const& chunk = static_cast<LevelChunkPacket const&>(*packet);
                covered           = *chunk.mDimensionId == job.dimension && columns.contains(*chunk.mPos);
            }
        }
        if (!covered) keptPackets.push_back(std::move(pending));
    }
    mPendingGamePackets      = std::move(keptPackets);
    mDeferredCompletedChunks = std::move(keptDeferred);
}

void Recorder::dropPendingSpawnPackets() {
    std::scoped_lock            lock(mPendingGamePacketsMutex);
    [[maybe_unused]] auto const dropped = std::erase_if(mPendingGamePackets, [](auto const& pending) {
        switch (static_cast<MinecraftPacketIds>(pending.mPacketId)) {
        case MinecraftPacketIds::AddActor:
        case MinecraftPacketIds::AddItemActor:
        case MinecraftPacketIds::AddPainting:
        case MinecraftPacketIds::AddPlayer:
            return true;
        default:
            return false;
        }
    });
}

bool Recorder::writeCreateLocalPlayer() {
    if (!mAsyncReplaySaver || !mSnapshotLocalPlayerPayload) {
        failRecording("Recorded local player snapshot data is unavailable");
        return false;
    }

    auto payload = *mSnapshotLocalPlayerPayload;
    if (!mAsyncReplaySaver->submit([payload = std::move(payload)](ReplayWriter& writer) {
            auto& action = ActionCreateLocalPlayer::getInstance();
            writer.startAction(action);
            writer.mStream.write(payload.data(), payload.size());
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the recorded local player snapshot"));
        return false;
    }
    return true;
}

bool Recorder::writeSnapshot() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a snapshot");
        return false;
    }
    if (!mSnapshotView || !mSnapshotCamera || !mSnapshotDimension) {
        failRecording("Replay snapshot context is unavailable");
        return false;
    }

    // NaN would never compare equal to its own copy when the replay re-reads the context.
    auto const finiteOrZero = [](float value) { return std::isfinite(value) ? value : 0.0f; };

    PlaybackSnapshotContext context;
    context.dimensionId        = mSnapshotDimension->id.mValue;
    context.dimensionMinHeight = mSnapshotDimension->minHeight;
    context.dimensionMaxHeight = mSnapshotDimension->maxHeight;
    context.x                  = mSnapshotView->x;
    context.y                  = mSnapshotView->y;
    context.z                  = mSnapshotView->z;
    context.yaw                = mSnapshotView->yaw;
    context.pitch              = mSnapshotView->pitch;
    context.cameraOffsetX      = finiteOrZero(mSnapshotCamera->x);
    context.cameraOffsetY      = finiteOrZero(mSnapshotCamera->y);
    context.cameraOffsetZ      = finiteOrZero(mSnapshotCamera->z);
    context.fov                = finiteOrZero(mSnapshotCamera->fov);
    context.hasView            = mSnapshotCamera->hasView;
    context.viewX              = finiteOrZero(mSnapshotCamera->viewX);
    context.viewY              = finiteOrZero(mSnapshotCamera->viewY);
    context.viewZ              = finiteOrZero(mSnapshotCamera->viewZ);
    context.viewYaw            = finiteOrZero(mSnapshotCamera->viewYaw);
    context.viewPitch          = finiteOrZero(mSnapshotCamera->viewPitch);
    context.dimensionName      = mSnapshotDimension->name;
    if (!mAsyncReplaySaver->submit([context](ReplayWriter& writer) {
            writer.startSnapshot();
            auto& action = ActionSnapshotContext::getInstance();
            writer.startAction(action);
            writeSnapshotContext(writer.mStream, context);
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot context"));
        return false;
    }

    // Packs are zipped on their own thread so the replay writer is not stalled by thousands of small files.
    if (mPackWorker) {
        auto packs = packs::collectServerPacks(mSnapshotConfigurationPackets);
        if (!packs.announced.empty()) mPackWorker->submit(std::move(packs));
    }

    if (!mSnapshotConfigurationPackets.empty()
        && !mAsyncReplaySaver->writeConfigurationPackets(std::move(mSnapshotConfigurationPackets))) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue replay snapshot configuration packets"));
        return false;
    }

    if (!writeCreateLocalPlayer()) return false;

    std::vector<AsyncReplaySaver::GamePacket> gamePackets;
    gamePackets.reserve(mSnapshotLevelChunks.size() + mSnapshotSubChunks.size() + mSnapshotEntityPackets.size());

    for (auto const& packet : mSnapshotLevelChunks) gamePackets.emplace_back(packet);
    for (auto const& packet : mSnapshotSubChunks) gamePackets.emplace_back(packet);
    for (auto const& packet : mSnapshotEntityPackets) gamePackets.emplace_back(packet);

    if (!mAsyncReplaySaver->writeGamePackets(std::move(gamePackets))) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue generated snapshot packets"));
        return false;
    }
    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.endSnapshot(); })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot footer"));
        return false;
    }
    return true;
}

bool Recorder::writeTickBoundary() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a tick boundary");
        return false;
    }

    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) {
            writer.startAndFinishAction(ActionNextTick::getInstance());
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue a replay tick boundary"));
        return false;
    }
    mHasOpenChunk     = true;
    mOpenChunkHasData = true;

    ++mTicksInCurrentChunk;
    ++mWrittenTicks;
    if (!mThumbnailCaptureRequested && mWrittenTicks >= THUMBNAIL_CAPTURE_TICKS
        && mState.load(std::memory_order_acquire) == State::Recording) {
        auto clientInstance = ll::service::getClientInstance();
        if (clientInstance && clientInstance->isInWorldAndNotShowingAnyMenuScreens()) {
            if (auto* provider = mThumbnailCaptureProvider.load(std::memory_order_acquire)) {
                provider->requestReplayThumbnailCapture();
                mThumbnailCaptureRequested = true;
                mThumbnailRequestTick      = mWrittenTicks;
            }
        }
    }
    return true;
}

bool Recorder::flushGamePackets() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing game packets");
        return false;
    }

    std::vector<PlaybackSerializedGamePacket> pending;
    std::vector<DeferredCompletedChunk>       deferred;
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        pending.swap(mPendingGamePackets);
        deferred.swap(mDeferredCompletedChunks);
    }

    if (!deferred.empty()) resolveDeferredCompletedChunks(pending, deferred);
    if (pending.empty()) return true;
    std::vector<AsyncReplaySaver::GamePacket> gamePackets;
    gamePackets.reserve(pending.size());
    for (auto& packet : pending) gamePackets.emplace_back(std::move(packet));
    if (mAsyncReplaySaver->writeGamePackets(std::move(gamePackets))) return true;

    auto error = mAsyncReplaySaver->getError();
    failRecording(error.value_or("Unable to queue game packets"));
    return false;
}

bool Recorder::writeEntityMovements() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing entity movements");
        return false;
    }

    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        failRecording("The local player is not ready while writing entity movements");
        return false;
    }

    struct MovementRecord {
        ActorUniqueID       id;
        EntityMovementState state;
    };

    std::vector<MovementRecord> changed;
    // Reuse last tick's buckets instead of rebuilding the map every tick.
    auto& current = mCurrentEntityMovements;
    current.clear();
    auto actors = localPlayer->getLevel().getRuntimeActorList();
    changed.reserve(actors.size());
    current.reserve(actors.size());

    for (auto const* actor : actors) {
        if (!actor || !actor->isAlive() || actor->getDimensionId() != localPlayer->getDimensionId()) {
            continue;
        }

        auto position = actor->getPosition();
        auto rotation = actor->getRotation();

        auto const& entityContext = actor->getEntityContext();
        auto const  interpolator  = entityContext.tryGetComponent<MovementInterpolatorComponent>();
        if (interpolator) {
            if (interpolator->mPositionSteps > 0) position = *interpolator->mPos;
            if (interpolator->mRotationSteps > 0) rotation = *interpolator->mRot;
        }

        float headYaw = rotation.y;
        if (auto const headRotation = entityContext.tryGetComponent<ActorHeadRotationComponent>()) {
            headYaw = headRotation->mYHeadRot;
        }
        if (interpolator && interpolator->mHeadYawSteps > 0) headYaw = interpolator->mHeadYaw;

        float bodyYaw = rotation.y;
        if (auto const bodyRotation = entityContext.tryGetComponent<MobBodyRotationComponent>()) {
            bodyYaw = bodyRotation->mYBodyRot;
        }

        EntityMovementState state;
        state.values   = {position.x, position.y, position.z, rotation.x, rotation.y, headYaw, bodyYaw};
        state.onGround = actor->isOnGround();

        if (actor == localPlayer && !mRecordedLocalPlayerId) {
            failRecording("The recorded local player identity is unavailable while writing entity movements");
            return false;
        }
        auto const id       = actor == localPlayer ? *mRecordedLocalPlayerId : actor->getOrCreateUniqueID();
        auto       it       = current.emplace(id, state).first;
        auto       previous = mLastEntityMovements.find(id);
        if (previous == mLastEntityMovements.end() || !previous->second.sameAs(it->second)) {
            changed.push_back({id, it->second});
        }
    }
    mLastEntityMovements.swap(current);

    if (changed.empty()) return true;
    if (!mAsyncReplaySaver->submit([changed = std::move(changed)](ReplayWriter& writer) {
            auto& action = ActionMoveEntities::getInstance();
            writer.startAction(action);
            writer.mStream.writeVarInt(0, nullptr, nullptr);
            writer.mStream.writeVarInt(static_cast<int>(changed.size()), nullptr, nullptr);
            for (auto const& movement : changed) {
                writer.mStream.writeVarInt64(movement.id.rawID, nullptr, nullptr);
                for (float const value : movement.state.values) writer.mStream.writeFloat(value, nullptr, nullptr);
                writer.mStream.writeBool(movement.state.onGround, nullptr, nullptr);
            }
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue entity movements"));
        return false;
    }
    return true;
}

bool Recorder::writeLocalCameraOffset() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing the local camera offset");
        return false;
    }

    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        failRecording("The local player is not ready while writing the local camera offset");
        return false;
    }

    auto const sample = captureLocalCameraSample(*localPlayer);

    if (!mAsyncReplaySaver->submit([sample](ReplayWriter& writer) {
            auto& action = ActionLocalCameraOffset::getInstance();
            writer.startAction(action);
            writer.mStream.writeFloat(sample.x, nullptr, nullptr);
            writer.mStream.writeFloat(sample.y, nullptr, nullptr);
            writer.mStream.writeFloat(sample.z, nullptr, nullptr);
            writer.mStream.writeFloat(sample.fov, nullptr, nullptr);
            writer.mStream.writeBool(sample.hasView, nullptr, nullptr);
            writer.mStream.writeFloat(sample.viewX, nullptr, nullptr);
            writer.mStream.writeFloat(sample.viewY, nullptr, nullptr);
            writer.mStream.writeFloat(sample.viewZ, nullptr, nullptr);
            writer.mStream.writeFloat(sample.viewYaw, nullptr, nullptr);
            writer.mStream.writeFloat(sample.viewPitch, nullptr, nullptr);
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the local camera offset"));
        return false;
    }
    return true;
}

namespace {

// Frames buffered while no tick runs (single-player pause menu, a hitch); beyond it the tick's frames are discarded.
constexpr size_t MaxBufferedLocalViewFrames = 512;
// Degrees a frame must leave the straight line between the two ticks for that tick to store its frames.
constexpr float LocalViewCurveThreshold = 0.01f;

} // namespace

void Recorder::recordLocalViewFrame(float alpha) {
    if (mLocalViewRate <= 20 || mState.load(std::memory_order_acquire) != State::Recording) {
        std::scoped_lock lock(mLocalViewMutex);
        mLocalViewFrames.clear();
        mLocalViewGap = true;
        return;
    }

    auto             clientInstance = ll::service::getClientInstance();
    auto*            localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    std::scoped_lock lock(mLocalViewMutex);
    if (!localPlayer || mLocalViewFrames.size() >= MaxBufferedLocalViewFrames) {
        mLocalViewGap = true;
        return;
    }
    auto const rotation = localPlayer->getRotation();
    if (!std::isfinite(alpha) || !std::isfinite(rotation.x) || !std::isfinite(rotation.y)) return;
    mLocalViewFrames.push_back({alpha, rotation.x, rotation.y});
}

void Recorder::discardLocalViewFrames() {
    std::scoped_lock lock(mLocalViewMutex);
    mLocalViewFrames.clear();
    mLocalViewGap = true;
    mLocalViewPrevious.reset();
}

bool Recorder::writeLocalViewSamples() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing the local view samples");
        return false;
    }

    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer || mLocalViewRate <= 20) {
        discardLocalViewFrames();
        return true;
    }

    std::vector<LocalViewFrame>   frames;
    std::optional<LocalViewFrame> previous;
    bool                          gap{};
    auto const                    rotation = localPlayer->getRotation();
    LocalViewFrame const          current{1.0f, rotation.x, rotation.y};
    {
        std::scoped_lock lock(mLocalViewMutex);
        frames.swap(mLocalViewFrames);
        gap      = std::exchange(mLocalViewGap, false);
        previous = std::exchange(mLocalViewPrevious, current);
    }
    if (gap || !previous || frames.empty()) return true;

    // A frame rendered exactly on a tick repeats that tick's pose.
    std::erase_if(frames, [](LocalViewFrame const& frame) {
        return !(frame.fraction > 0.0f && frame.fraction < 1.0f);
    });
    std::stable_sort(frames.begin(), frames.end(), [](auto const& a, auto const& b) {
        return a.fraction < b.fraction;
    });
    frames.erase(
        std::unique(
            frames.begin(),
            frames.end(),
            [](auto const& a, auto const& b) { return a.fraction == b.fraction; }
        ),
        frames.end()
    );

    float const yawSpan = std::remainder(current.yaw - previous->yaw, 360.0f);
    bool const  curved  = std::ranges::any_of(frames, [&](LocalViewFrame const& frame) {
        float const lineYaw   = previous->yaw + yawSpan * frame.fraction;
        float const linePitch = std::lerp(previous->pitch, current.pitch, frame.fraction);
        return std::abs(std::remainder(frame.yaw - lineYaw, 360.0f)) > LocalViewCurveThreshold
            || std::abs(frame.pitch - linePitch) > LocalViewCurveThreshold;
    });
    if (!curved) return true;

    // Keep the frame nearest each evenly spaced point inside the tick.
    int const                   slots = mLocalViewRate / 20;
    std::vector<LocalViewFrame> picked;
    picked.reserve(static_cast<size_t>(slots - 1));
    for (int slot = 1; slot < slots; ++slot) {
        float const target  = static_cast<float>(slot) / static_cast<float>(slots);
        auto const  nearest = std::ranges::min_element(frames, {}, [&](LocalViewFrame const& frame) {
            return std::abs(frame.fraction - target);
        });
        if (picked.empty() || picked.back().fraction != nearest->fraction) picked.push_back(*nearest);
    }
    frames = std::move(picked);
    ++mLocalViewTicks;
    mLocalViewSamples += frames.size();

    if (!mAsyncReplaySaver->submit([frames = std::move(frames)](ReplayWriter& writer) {
            auto& action = ActionLocalViewSamples::getInstance();
            writer.startAction(action);
            writer.mStream.writeVarInt(static_cast<int>(frames.size()), nullptr, nullptr);
            for (auto const& frame : frames) {
                writer.mStream.writeFloat(frame.fraction, nullptr, nullptr);
                writer.mStream.writeFloat(frame.pitch, nullptr, nullptr);
                writer.mStream.writeFloat(frame.yaw, nullptr, nullptr);
            }
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the local view samples"));
        return false;
    }
    return true;
}

bool Recorder::writeLocalPlayerState() {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer || !mRecordedLocalPlayerRuntimeId) {
        failRecording("The local player is not ready while writing replayable player state");
        return false;
    }

    auto recordChanged = [this](Packet const& packet, std::optional<std::string>& previous) {
        // Reused every tick so the unchanged case does not allocate.
        auto& serialized = mLocalPlayerDataBuffer;
        serialized.clear();
        packet.write(serialized);
        if (previous && *previous == serialized.mBuffer) return false;
        previous = serialized.mBuffer;
        recordGamePacket(packet);
        return true;
    };

    auto&              entityContext = localPlayer->getEntityContext();
    auto*              properties    = entityContext.tryGetComponent<PropertyComponent>().as_ptr();
    SetActorDataPacket actorData(localPlayer->getRuntimeID(), *localPlayer->mEntityData, properties, 0, true);
    recordChanged(actorData, mLastLocalPlayerDataPacket);

    PlaybackSetEquipmentPacket equipment(*localPlayer, localPlayer->getRuntimeID(), localPlayer->getSelectedItemSlot());
    auto const* previousEquipment   = mLastLocalPlayerEquipmentPacket ? &*mLastLocalPlayerEquipmentPacket : nullptr;
    auto        equipmentPackets    = equipment.createPackets(previousEquipment);
    mLastLocalPlayerEquipmentPacket = std::move(equipment);
    for (auto const& packet : equipmentPackets) {
        recordGamePacket(*packet);
    }

    // The replay observer's HUD reads its own hotbar, armor, attributes and game mode, so they are recorded as well.
    // Unchanged slots used to be serialized twice per tick just to find out they had not changed.
    auto recordSlot = [&](size_t index, ContainerID container, int slot, ItemStack const& item) {
        if (sameHudItem(item, mLastLocalHudItems[index])) return;
        recordChanged(*makeHudSlotPacket(container, slot, item), mLastLocalHudPackets[index]);
        mLastLocalHudItems[index] = item;
    };
    for (int slot = 0; slot < HudHotbarSlots; ++slot) {
        recordSlot(
            static_cast<size_t>(slot),
            ContainerID::Inventory,
            slot,
            localPlayer->getSupplies().getItem(slot, ContainerID::Inventory)
        );
    }
    for (int slot = 0; slot < HudArmorSlots; ++slot) {
        recordSlot(
            HudArmorKey + static_cast<size_t>(slot),
            ContainerID::Armor,
            slot,
            localPlayer->getArmor(static_cast<SharedTypes::Legacy::ArmorSlot>(slot))
        );
    }
    recordSlot(HudOffhandKey, ContainerID::Offhand, 0, localPlayer->getOffhandSlot());

    int const selectedSlot = localPlayer->getSelectedItemSlot();
    if (selectedSlot != mLastLocalSelectedSlot) {
        recordChanged(*makeHudSelectedSlotPacket(*localPlayer), mLastLocalHudPackets[HudSelectedKey]);
        mLastLocalSelectedSlot = selectedSlot;
    }

    std::array<float, 18> attributeValues{};
    auto const            attributes = {
        &SharedAttributes::HEALTH(),
        &SharedAttributes::ABSORPTION(),
        &Player::HUNGER(),
        &Player::SATURATION(),
        &Player::LEVEL(),
        &Player::EXPERIENCE(),
    };
    size_t valueIndex = 0;
    for (auto const* attribute : attributes) {
        auto const instance = localPlayer->getAttribute(*attribute);
        if (instance.mPtr) {
            attributeValues[valueIndex]     = instance.mPtr->mCurrentMinValue;
            attributeValues[valueIndex + 1] = instance.mPtr->mCurrentMaxValue;
            attributeValues[valueIndex + 2] = instance.mPtr->mCurrentValue;
        }
        valueIndex += 3;
    }
    if (!mLastLocalAttributeValuesValid
        || std::memcmp(attributeValues.data(), mLastLocalAttributeValues.data(), sizeof(attributeValues)) != 0) {
        recordChanged(
            *makeHudAttributePacket(*localPlayer, *mRecordedLocalPlayerRuntimeId),
            mLastLocalHudPackets[HudAttributeKey]
        );
        mLastLocalAttributeValues      = attributeValues;
        mLastLocalAttributeValuesValid = true;
    }

    if (mRecordedLocalPlayerId) {
        int const gameType = static_cast<int>(localPlayer->getPlayerGameType());
        if (gameType != mLastLocalGameTypeValue) {
            recordChanged(*makeHudGameTypePacket(*localPlayer, *mRecordedLocalPlayerId), mLastLocalGameTypePacket);
            mLastLocalGameTypeValue = gameType;
        }
    }

    for (size_t hand = 0; hand < mLastLocalHeldMapIds.size(); ++hand) {
        auto const& item  = hand == 0 ? localPlayer->getCarriedItem() : localPlayer->getOffhandSlot();
        auto const  mapId = heldMapId(item);
        if (mapId == mLastLocalHeldMapIds[hand]) continue;
        mLastLocalHeldMapIds[hand] = mapId;
        if (!mapId) continue;
        if (auto mapPacket = makeHeldMapPacket(*localPlayer, *mapId)) recordGamePacket(*mapPacket);
    }

    bool const swinging  = localPlayer->mSwinging;
    int const  swingTime = localPlayer->mSwingTime;
    if (!swinging) {
        mLastLocalPlayerSwingTime.reset();
    } else {
        if (!mLastLocalPlayerSwingTime || swingTime < *mLastLocalPlayerSwingTime) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::Animate);
            if (!packet) {
                failRecording("Unable to create the local-player swing packet");
                return false;
            }
            auto& animate        = static_cast<AnimatePacket&>(*packet);
            animate.mRuntimeId   = localPlayer->getRuntimeID();
            animate.mAction      = AnimatePacketPayload::Action::Swing;
            animate.mData        = 0.0f;
            animate.mSwingSource = ActorSwingSource::None;
            recordGamePacket(animate);
        }
        mLastLocalPlayerSwingTime = swingTime;
    }

    // Answering or closing a form is never echoed by the server, so the close is recorded as the packet a server sends
    // to close forms.
    bool const formOpen = clientInstance->getTopScreenName() == "third_party_server_screen";
    if (mLocalFormOpen && !formOpen) {
        if (auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ClientboundCloseScreen)) {
            recordGamePacket(*packet);
        }
    }
    mLocalFormOpen = formOpen;
    return true;
}

void Recorder::recordReceivedPacket(Packet const& packet, std::string_view payload) {
    switch (packet.getId()) {
    // Spawns are rebuilt from the entity the handler creates.
    case MinecraftPacketIds::AddActor:
    case MinecraftPacketIds::AddItemActor:
        return;
    case MinecraftPacketIds::FullChunkData:
        recordLevelChunkPacket(static_cast<LevelChunkPacket const&>(packet), payload);
        return;
    default:
        recordGamePacket(packet, payload);
    }
}

void Recorder::recordHandledPacket(Packet const& packet) {
    switch (packet.getId()) {
    case MinecraftPacketIds::AddActor:
        recordSpawnedActor(*static_cast<AddActorPacket const&>(packet).mRuntimeId, packet);
        return;
    case MinecraftPacketIds::AddItemActor:
        recordSpawnedActor(*static_cast<AddItemActorPacket const&>(packet).mRuntimeId, packet);
        return;
    default:
        return;
    }
}

void Recorder::recordSpawnedActor(ActorRuntimeID runtimeId, Packet const& fallbackPacket) {
    if (!isActive()) return;

    auto  client      = ll::service::getClientInstance();
    auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
    auto* actor       = localPlayer ? localPlayer->getLevel().getRuntimeEntity(runtimeId, false) : nullptr;
    if (!actor) {
        getLogger().warn(
            "Unable to find spawned actor {} for timeline reconstruction; recording the original {}",
            runtimeId.rawID,
            fallbackPacket.getName()
        );
        recordGamePacket(fallbackPacket);
        return;
    }

    auto spawnPacket = actor->tryCreateAddActorPacket();
    if (!spawnPacket) {
        getLogger().warn(
            "Unable to reconstruct a replayable spawn packet for actor {}; recording the original {}",
            runtimeId.rawID,
            fallbackPacket.getName()
        );
        recordGamePacket(fallbackPacket);
    } else {
        recordGamePacket(*spawnPacket);
    }

    if (!actor->hasCategory(ActorCategory::Mob)) return;

    PlaybackSetEquipmentPacket equipment(*actor, runtimeId);
    for (auto const& packet : equipment.createPackets()) {
        recordGamePacket(*packet);
    }
}

void Recorder::ensureRecordedItemRegistry(Level& level) {
    auto const packetId = static_cast<int32_t>(MinecraftPacketIds::ItemRegistryPacket);
    bool       recorded = false;
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        recorded = mConfigurationPacketIndices.contains(packetId);
    }
    if (!recorded) {
        // Fallback for sessions joined before recording hooks were installed. It lacks the server's item components.
        try {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ItemRegistryPacket);
            if (!packet) throw std::runtime_error("Unable to create the item registry packet");
            *static_cast<ItemRegistryPacket&>(*packet).mItems = ItemData::fromItemRegistry(level.getItemRegistry());
            mRecordingSyntheticItemRegistry                   = true;
            recordConfigurationPacket(*packet, describePacketLifecycle(MinecraftPacketIds::ItemRegistryPacket));
            mRecordingSyntheticItemRegistry = false;
        } catch (std::exception const& error) {
            mRecordingSyntheticItemRegistry = false;
            getLogger().error("Unable to record the item registry: {}", error.what());
        } catch (...) {
            mRecordingSyntheticItemRegistry = false;
            getLogger().error("Unable to record the item registry");
        }
    }

    char const* source = "none";
    if (mItemRegistrySource == ItemRegistrySource::Original) source = "original";
    else if (mItemRegistrySource == ItemRegistrySource::Synthetic) source = "synthetic";
    getLogger().info(
        "Recorded item registry items={} custom={} componentBased={} source={}",
        mItemRegistryItems,
        mItemRegistryCustom,
        mItemRegistryComponentBased,
        source
    );
}

void Recorder::recordConfigurationPacket(
    Packet const&                   packet,
    PacketLifecycleSemantics const& semantics,
    std::string_view                payload
) {
    try {
        if (!semantics.isConfiguration()) return;

        auto const packetType = packet.getId();

        if (packetType == MinecraftPacketIds::ItemRegistryPacket) {
            int total          = 0;
            int custom         = 0;
            int componentBased = 0;
            for (auto const& item : *static_cast<ItemRegistryPacket const&>(packet).mItems) {
                ++total;
                if (!item.mName->getString().starts_with("minecraft:")) ++custom;
                if (static_cast<bool>(item.mIsComponentBased)) ++componentBased;
            }
            mItemRegistryItems          = total;
            mItemRegistryCustom         = custom;
            mItemRegistryComponentBased = componentBased;
            mItemRegistrySource =
                mRecordingSyntheticItemRegistry ? ItemRegistrySource::Synthetic : ItemRegistrySource::Original;
        }

        PlaybackBuffer stream;
        if (payload.empty()) packet.write(stream);
        else stream.mBuffer.assign(payload);

        auto const       packetId = static_cast<int32_t>(packetType);
        std::scoped_lock lock(mPendingGamePacketsMutex);

        if (semantics.startsConfigurationEpoch) {
            mConfigurationPackets.clear();
            mConfigurationPacketIndices.clear();
            // The cached item table went with the epoch; a later synthetic one must not inherit its label.
            mItemRegistrySource         = ItemRegistrySource::None;
            mItemRegistryItems          = 0;
            mItemRegistryCustom         = 0;
            mItemRegistryComponentBased = 0;
        }

        if (semantics.supersedes) {
            auto const supersededPacketId = static_cast<int32_t>(*semantics.supersedes);
            mConfigurationPackets.erase(
                std::remove_if(
                    mConfigurationPackets.begin(),
                    mConfigurationPackets.end(),
                    [supersededPacketId](auto const& cached) { return cached.mPacketId == supersededPacketId; }
                ),
                mConfigurationPackets.end()
            );
            mConfigurationPacketIndices.clear();
            for (size_t index = 0; index < mConfigurationPackets.size(); ++index) {
                mConfigurationPacketIndices.insert_or_assign(mConfigurationPackets[index].mPacketId, index);
            }
        }

        auto const existing = mConfigurationPacketIndices.find(packetId);
        if (existing != mConfigurationPacketIndices.end()) {
            auto& cached = mConfigurationPackets[existing->second];
            if (cached.mPayload == stream.mBuffer) return;
            if (semantics.keepsSequence()) {
                mConfigurationPacketIndices.insert_or_assign(packetId, mConfigurationPackets.size());
                mConfigurationPackets.push_back({packetId, std::move(stream.mBuffer)});
            } else {
                cached.mPayload = std::move(stream.mBuffer);
            }
            return;
        }

        mConfigurationPacketIndices.emplace(packetId, mConfigurationPackets.size());
        mConfigurationPackets.push_back({packetId, std::move(stream.mBuffer)});
    } catch (std::exception const& exception) {
        getLogger().error("Unable to serialize replay configuration packet {}: {}", packet.getName(), exception.what());
    } catch (...) {
        getLogger().error("Unable to serialize replay configuration packet {}", packet.getName());
    }
}

void Recorder::recordCompletedChunk(ChunkPos const& pos, Dimension const& dimension) {
    if (mState.load(std::memory_order_acquire) != State::Recording) return;

    auto const dimensionId = dimension.getDimensionId();
    {
        std::scoped_lock lock(mPendingPortableChunksMutex);
        auto const       byDimension = mPendingPortableChunks.find(dimensionId);
        if (byDimension == mPendingPortableChunks.end() || !byDimension->second.contains(pos)) return;
    }

    auto chunk = dimension.getChunkSource().getExistingChunk(pos);
    if (!chunk || chunk->mIsEmptyClientChunk
        || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
        return;
    }

    auto const dimensionMinHeight = static_cast<int>(dimension.mHeightRange->mMin);
    auto const dimensionMaxHeight = static_cast<int>(dimension.mHeightRange->mMax);
    auto const subChunkCount      = static_cast<size_t>(dimension.getHeightInSubchunks());
    if (dimensionMinHeight % 16 != 0 || dimensionMaxHeight <= dimensionMinHeight
        || static_cast<size_t>((dimensionMaxHeight - dimensionMinHeight) / 16) != subChunkCount
        || chunk->mSubChunks->empty()) {
        return;
    }

    // This runs inside the engine's chunk-completion continuation on the main thread, once per column. Only the
    // timeline slot is reserved here; flushGamePackets serializes every column completed this tick in parallel.
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.push_back({DeferredChunkPacketId, {}});
        mDeferredCompletedChunks.push_back({pos, dimensionId, dimensionMinHeight / 16, std::move(chunk)});
    }

    std::scoped_lock lock(mPendingPortableChunksMutex);
    auto const       byDimension = mPendingPortableChunks.find(dimensionId);
    if (byDimension == mPendingPortableChunks.end()) return;
    byDimension->second.erase(pos);
    if (byDimension->second.empty()) mPendingPortableChunks.erase(byDimension);
}

bool Recorder::serializeDeferredCompletedChunk(
    DeferredCompletedChunk const& job,
    Block const&                  air,
    SaveContext const&            saveContext,
    std::string&                  levelChunkPayload,
    std::string&                  subChunkPayload
) {
    auto const& pos   = job.pos;
    auto const& chunk = *job.chunk;
    // The column may have been discarded later in the tick; the shared_ptr keeps it alive but its storage is stale.
    if (chunk.mIsEmptyClientChunk || chunk.mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
        return false;
    }

    auto levelBase = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
    auto subBase   = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
    if (!levelBase || !subBase || levelBase->getId() != MinecraftPacketIds::FullChunkData
        || subBase->getId() != MinecraftPacketIds::SubChunkPacket) {
        return false;
    }

    auto& level                          = static_cast<LevelChunkPacket&>(*levelBase);
    level.mPos                           = pos;
    level.mDimensionId                   = job.dimensionId;
    level.mCacheEnabled                  = false;
    level.mSubChunksCount                = 0;
    level.mClientNeedsToRequestSubchunks = true;
    level.mClientRequestSubChunkLimit    = -1;
    level.mCacheMetadata->clear();

    BinaryStream     levelPayload;
    VarIntDataOutput levelOutput(levelPayload);
    chunk.serializeBiomes(levelOutput);
    chunk.serializeBorderBlocks(levelOutput);
    level.mSerializedChunk = std::move(levelPayload.mBuffer);

    auto const& subChunks         = *chunk.mSubChunks;
    auto&       subChunkPacket    = static_cast<SubChunkPacket&>(*subBase);
    subChunkPacket.mCacheEnabled  = false;
    subChunkPacket.mDimensionType = job.dimensionId;
    subChunkPacket.mCenterPos     = SubChunkPos{pos.x, job.minimumSubChunkY, pos.z};
    subChunkPacket.mSubChunkData->clear();
    subChunkPacket.mSubChunkData->reserve(subChunks.size());

    for (auto const& subChunk : subChunks) {
        if (subChunk.isPlaceHolderSubChunk()) continue;
        if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
            && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished
            && subChunk.mSubChunkState != SubChunk::SubChunkState::ProcessedSubChunk) {
            continue;
        }

        int const actualAbsoluteY = static_cast<int>(static_cast<schar>(subChunk.mAbsoluteIndex));
        int const relativeY       = actualAbsoluteY - job.minimumSubChunkY;
        if (relativeY < std::numeric_limits<schar>::min() || relativeY > std::numeric_limits<schar>::max()) continue;

        BinaryStream serializedSubChunk;
        bool const   allAir = subChunk.isUniform(air);
        if (!allAir) {
            VarIntDataOutput subChunkOutput(serializedSubChunk);
            subChunk.serialize(subChunkOutput, false);
        }
        VarIntDataOutput blockActorOutput(serializedSubChunk);
        chunk.serializeBlockEntitiesForSubChunk(
            blockActorOutput,
            SubChunkPos{pos.x, actualAbsoluteY, pos.z},
            saveContext
        );

        SubChunkPacket::SubChunkPosOffset offset{};
        offset.mY         = static_cast<schar>(relativeY);
        auto const result = allAir ? SubChunkPacket::SubChunkRequestResult::SuccessAllAir
                                   : SubChunkPacket::SubChunkRequestResult::Success;
        subChunkPacket.mSubChunkData->emplace_back(offset, result);
        auto& data               = subChunkPacket.mSubChunkData->back();
        data.mSerializedSubChunk = std::move(serializedSubChunk.mBuffer);
        data.mBlobId             = 0;
        chunk.populateHeightMapDataForSubChunkPacket(static_cast<short>(actualAbsoluteY), data);
    }

    PlaybackBuffer levelStream;
    level.write(levelStream);
    levelChunkPayload = std::move(levelStream.mBuffer);

    subChunkPayload.clear();
    if (!subChunkPacket.mSubChunkData->empty()) {
        PlaybackBuffer subChunkStream;
        subChunkPacket.write(subChunkStream);
        subChunkPayload = std::move(subChunkStream.mBuffer);
    }
    return true;
}

void Recorder::resolveDeferredCompletedChunks(
    std::vector<PlaybackSerializedGamePacket>& pending,
    std::vector<DeferredCompletedChunk>&       deferred
) {

    struct Result {
        std::string levelChunk;
        std::string subChunk;
        bool        ok{};
    };
    std::vector<Result> results(deferred.size());

    // Block reads tolerate concurrent workers because SubChunk::_setBlock retires replaced storage through a delayed
    // free queue; the block actor list only changes on the main thread, which waits here like in a chunk snapshot.
    auto air = Block::tryGetFromRegistry(BedrockBlockNames::Air());
    if (air) {
        std::atomic<size_t> nextJob{0};
        auto                serializeJobs = [&deferred, &results, &nextJob, air]() {
            auto saveContext = SaveContextFactory::createNetworkSaveContext();
            if (!saveContext) return;
            for (size_t index = nextJob.fetch_add(1, std::memory_order_relaxed); index < deferred.size();
                 index        = nextJob.fetch_add(1, std::memory_order_relaxed)) {
                auto const& job    = deferred[index];
                auto&       result = results[index];
                try {
                    result.ok =
                        serializeDeferredCompletedChunk(job, *air, *saveContext, result.levelChunk, result.subChunk);
                } catch (std::exception const& exception) {
                    getLogger()
                        .warn("Unable to capture completed chunk ({}, {}): {}", job.pos.x, job.pos.z, exception.what());
                } catch (...) {
                    getLogger().warn("Unable to capture completed chunk ({}, {})", job.pos.x, job.pos.z);
                }
            }
        };

        runParallel(deferred.size() - 1, serializeJobs);
    }

    // Expand each reserved slot in place, deduplicating the way recordGamePacket would have when the column completed.
    std::vector<PlaybackSerializedGamePacket> expanded;
    expanded.reserve(pending.size() + deferred.size());
    uint64_t levelChunkCount = 0;
    uint64_t subChunkCount   = 0;
    auto     append          = [&expanded](int32_t packetId, std::string&& payload) {
        for (auto const& [existingId, existingPayload] : expanded) {
            if (existingId == packetId && existingPayload == payload) return false;
        }
        expanded.push_back({packetId, std::move(payload)});
        return true;
    };

    size_t nextDeferred = 0;
    for (auto& packet : pending) {
        if (packet.mPacketId != DeferredChunkPacketId) {
            // A native chunk packet recorded after a reserved slot was only deduplicated against the empty placeholder.
            bool const chunkPacket = packet.mPacketId == static_cast<int32_t>(MinecraftPacketIds::FullChunkData)
                                  || packet.mPacketId == static_cast<int32_t>(MinecraftPacketIds::SubChunkPacket);
            if (chunkPacket && nextDeferred > 0) {
                append(packet.mPacketId, std::move(packet.mPayload));
            } else {
                expanded.push_back(std::move(packet));
            }
            continue;
        }
        if (nextDeferred >= results.size()) continue;
        auto& result = results[nextDeferred++];
        if (!result.ok) continue;
        if (append(static_cast<int32_t>(MinecraftPacketIds::FullChunkData), std::move(result.levelChunk))) {
            ++levelChunkCount;
        }
        if (!result.subChunk.empty()
            && append(static_cast<int32_t>(MinecraftPacketIds::SubChunkPacket), std::move(result.subChunk))) {
            ++subChunkCount;
        }
    }
    pending = std::move(expanded);

    std::scoped_lock lock(mPendingGamePacketsMutex);
    mRecordedGamePacketCounts[static_cast<int32_t>(MinecraftPacketIds::FullChunkData)]  += levelChunkCount;
    mRecordedGamePacketCounts[static_cast<int32_t>(MinecraftPacketIds::SubChunkPacket)] += subChunkCount;
}

void Recorder::recordLevelChunkPacket(LevelChunkPacket const& packet, std::string_view payload) {
    auto const state = mState.load(std::memory_order_acquire);
    if (state != State::Recording && state != State::Closing) return;

    auto const& pos         = *packet.mPos;
    auto const  dimensionId = *packet.mDimensionId;
    markSnapshotColumnDirty(pos);
    bool const requestMode  = static_cast<bool>(packet.mClientNeedsToRequestSubchunks);
    bool const cacheEnabled = static_cast<bool>(packet.mCacheEnabled);
    if (requestMode && !cacheEnabled) {
        {
            std::scoped_lock lock(mPendingPortableChunksMutex);
            auto const       byDimension = mPendingPortableChunks.find(dimensionId);
            if (byDimension != mPendingPortableChunks.end()) {
                byDimension->second.erase(pos);
                if (byDimension->second.empty()) mPendingPortableChunks.erase(byDimension);
            }
        }
        recordGamePacket(packet, payload);
        return;
    }

    std::scoped_lock lock(mPendingPortableChunksMutex);
    mPendingPortableChunks[dimensionId].emplace(pos);
}

void Recorder::markSnapshotColumnDirty(ChunkPos const& pos) {
    std::scoped_lock lock(mSnapshotDirtyColumnsMutex);
    mSnapshotDirtyColumns.insert(pos);
}

void Recorder::markSnapshotColumnsDirty(Packet const& packet) {
    auto mark = [this](BlockPos const& pos) { markSnapshotColumnDirty(ChunkPos{pos}); };

    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock:
        mark(*static_cast<UpdateBlockPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::UpdateBlockSynced:
        mark(*static_cast<UpdateBlockSyncedPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::UpdateSubChunkBlocks: {
        auto const& changed = *static_cast<UpdateSubChunkBlocksPacket const&>(packet).mBlocksChanged;
        mark(*static_cast<UpdateSubChunkBlocksPacket const&>(packet).mSubChunkBlockPosition);
        for (auto const* blocks : {&*changed.mStandards, &*changed.mExtras}) {
            for (auto const& info : *blocks) mark(*info.mPos);
        }
        break;
    }
    case MinecraftPacketIds::BlockActorData:
        mark(*static_cast<BlockActorDataPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::TileEvent:
        mark(*static_cast<BlockEventPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::FullChunkData:
        markSnapshotColumnDirty(*static_cast<LevelChunkPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::SubChunkPacket: {
        auto const& subChunkPacket = static_cast<SubChunkPacket const&>(packet);
        auto const& center         = *subChunkPacket.mCenterPos;
        for (auto const& entry : *subChunkPacket.mSubChunkData) {
            auto const& offset = *entry.mSubChunkPosOffset;
            markSnapshotColumnDirty(
                ChunkPos{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)}
            );
        }
        break;
    }
    default:
        break;
    }
}

void Recorder::resetSnapshotColumnCache() {
    mSnapshotColumnCache.clear();
    mSnapshotColumnCacheDimension.reset();
    mSnapshotCarriedDirtyColumns.clear();
    std::scoped_lock lock(mSnapshotDirtyColumnsMutex);
    mSnapshotDirtyColumns.clear();
}

void Recorder::recordGamePacket(Packet const& packet, std::string_view payload) {
    auto const packetId = packet.getId();
    if (mState.load(std::memory_order_acquire) != State::Idle) markSnapshotColumnsDirty(packet);
    auto const semantics = describePacketLifecycle(packetId);
    switch (semantics.lifecycle) {
    case PacketLifecycle::Ignore:
        return;
    case PacketLifecycle::PreWorldHandshake:
    case PacketLifecycle::SnapshotLatest:
    case PacketLifecycle::SnapshotSequence:
        recordConfigurationPacket(packet, semantics, payload);
        return;
    case PacketLifecycle::Timeline:
        break;
    }

    auto const state             = mState.load(std::memory_order_acquire);
    auto const recordingTimeline = state == State::Recording || state == State::Closing;
    if (!recordingTimeline) return;

    try {
        if (packetId == MinecraftPacketIds::AddActor
            && static_cast<AddActorPacket const&>(packet).mEntityData == nullptr) {
            return;
        }
        if (packetId == MinecraftPacketIds::AddItemActor
            && static_cast<AddItemActorPacket const&>(packet).mEntityData == nullptr) {
            return;
        }
        if (packetId == MinecraftPacketIds::FullChunkData
            && static_cast<bool>(static_cast<LevelChunkPacket const&>(packet).mCacheEnabled)) {
            return;
        }
        PlaybackBuffer stream;
        if (packetId == MinecraftPacketIds::SubChunkPacket) {
            auto const& subChunkPacket = static_cast<SubChunkPacket const&>(packet);
            if (static_cast<bool>(subChunkPacket.mCacheEnabled)) return;
            auto const& entries = *subChunkPacket.mSubChunkData;
            if (entries.empty()) return;

            // Copying the packet duplicates every serialized subchunk, so only filter when something must go.
            bool const allSuccessful = std::all_of(entries.begin(), entries.end(), [](auto const& entry) {
                return isSuccessfulSubChunkResult(
                    static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult)
                );
            });
            if (allSuccessful) {
                packet.write(stream);
            } else {
                auto filteredPacket = subChunkPacket;

                std::vector<SubChunkPacket::SubChunkPacketData> successfulEntries;
                successfulEntries.reserve(entries.size());
                for (auto const& entry : entries) {
                    if (isSuccessfulSubChunkResult(
                            static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult)
                        )) {
                        successfulEntries.emplace_back(entry);
                    }
                }
                if (successfulEntries.empty()) return;
                *filteredPacket.mSubChunkData = std::move(successfulEntries);
                filteredPacket.write(stream);
            }
        } else if (payload.empty()) {
            packet.write(stream);
        } else {
            stream.mBuffer.assign(payload);
        }

        if (packetId == MinecraftPacketIds::ChangeDimension) {
            auto const& change = static_cast<ChangeDimensionPacket const&>(packet);
            mDimensionTransitionTargetId.store(change.mDimensionId->mValue, std::memory_order_release);
            mDimensionTransitionPending.store(true, std::memory_order_release);
        }

        auto  clientInstance = ll::service::getClientInstance();
        auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;

        if (packetCarriesBlockNetworkIds(packetId)) {
            auto level = ll::service::getMultiPlayerLevel();
            if (level && !level->blockNetworkIdsAreHashes()) {
                auto                 portablePacket = MinecraftPackets::createPacket(packetId);
                ReadOnlyBinaryStream input(stream.mBuffer, false);
                if (!portablePacket || !portablePacket->read(input) || !input.ensureReadCompleted()) {
                    getLogger().warn("Unable to decode {} for block network ID normalization", packet.getName());
                } else if (makeBlockNetworkIdsPortable(*portablePacket, level->getBlockPalette())) {
                    PlaybackBuffer portableStream;
                    portablePacket->write(portableStream);
                    stream.mBuffer = std::move(portableStream.mBuffer);
                }
            }
        }

        if (localPlayer && mRecordedLocalPlayerId && mRecordedLocalPlayerRuntimeId && mRecordedLocalPlayerUuid
            && packetMayReferenceRecordedPlayer(packetId)
            && referencesRecordedPlayer(
                packet,
                localPlayer->getOrCreateUniqueID(),
                localPlayer->getRuntimeID(),
                localPlayer->getUuid()
            )) {
            auto                 remappedPacket = MinecraftPackets::createPacket(packetId);
            ReadOnlyBinaryStream input(stream.mBuffer, false);
            if (!remappedPacket || !remappedPacket->read(input) || !input.ensureReadCompleted()) {
                getLogger().warn("Unable to decode {} for recorded-player ID remapping", packet.getName());
            } else {
                if (remapRecordedPlayerReferences(
                        *remappedPacket,
                        localPlayer->getOrCreateUniqueID(),
                        localPlayer->getRuntimeID(),
                        localPlayer->getUuid(),
                        *mRecordedLocalPlayerId,
                        *mRecordedLocalPlayerRuntimeId,
                        *mRecordedLocalPlayerUuid
                    )) {
                    PlaybackBuffer remappedStream;
                    remappedPacket->write(remappedStream);
                    stream.mBuffer = std::move(remappedStream.mBuffer);
                }
            }
        }

        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.push_back({static_cast<int32_t>(packetId), std::move(stream.mBuffer)});
        auto& recordedCount = mRecordedGamePacketCounts[static_cast<int32_t>(packetId)];
        ++recordedCount;
    } catch (std::exception const& exception) {
        getLogger().error("Unable to serialize incoming game packet {}: {}", packet.getName(), exception.what());
    } catch (...) {
        getLogger().error("Unable to serialize incoming game packet {}", packet.getName());
    }
}

bool Recorder::finishCurrentChunk(bool close) {
    if (!mHasOpenChunk || !mOpenChunkHasData) return true;

    std::string chunkName = "chunk_" + std::to_string(mChunkIndex) + ".bin";

    PlaybackChunkMeta chunkMeta;
    chunkMeta.duration          = mTicksInCurrentChunk;
    chunkMeta.forcePlaySnapshot = mCurrentChunkForcePlaySnapshot;
    mMetadata.chunks.insert_or_assign(chunkName, chunkMeta);
    mMetadata.totalTicks = mWrittenTicks;

    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while finishing a replay chunk");
        return false;
    }

    if (!mAsyncReplaySaver->writeReplayChunk(chunkName, mMetadata.toJson())) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue a replay chunk for writing"));
        return false;
    }

    ++mChunkIndex;
    mTicksInCurrentChunk           = 0;
    mHasOpenChunk                  = false;
    mOpenChunkHasData              = false;
    mCurrentChunkForcePlaySnapshot = false;

    if (!close) {
        mNeedsInitialSnapshot = true;
    }
    return true;
}

void Recorder::failRecording(std::string_view reason) { cancelRecording(reason); }

void Recorder::cancelRecording(std::string_view reason) {
    getLogger().error("Recording cancelled: {}", reason);
    abortRecording();
}

void Recorder::abortRecording() {
    if (mPackWorker) {
        mPackWorker->cancel();
        mPackWorker.reset();
    }
    if (mAsyncReplaySaver) {
        mAsyncReplaySaver->cancel();
        mAsyncReplaySaver.reset();
    }

    mState                       = State::Idle;
    mNeedsInitialSnapshot        = true;
    mPendingDimensionSnapshot    = false;
    mDimensionTransitionPending  = false;
    mDimensionTransitionTargetId = 0;
    discardRotationSnapshot();
    resetChunkSnapshot();
    resetSnapshotColumnCache();
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    mLocalFormOpen = false;
    mLastLocalHudPackets.fill(std::nullopt);
    mLastLocalHudItems.fill(ItemStack());
    mLastLocalSelectedSlot         = -1;
    mLastLocalAttributeValuesValid = false;
    mLastLocalGameTypeValue        = -1;
    mLastLocalGameTypePacket.reset();
    mLastLocalHeldMapIds.fill(std::nullopt);
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
        mDeferredCompletedChunks.clear();
    }
    {
        std::scoped_lock lock(mPendingPortableChunksMutex);
        mPendingPortableChunks.clear();
    }
}

} // namespace playback::record
