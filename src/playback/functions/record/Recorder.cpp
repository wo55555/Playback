#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/ActorEventPacket.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/AnimatePacket.h"
#include "mc/network/packet/DimensionDataPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/LevelSoundEventPacket.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MobEffectPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketType.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/SetActorDataPacket.h"
#include "mc/network/packet/SetActorLinkPacket.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/TakeItemActorPacket.h"
#include "mc/network/packet/UpdateAttributesPacket.h"
#include "mc/network/packet/UpdatePlayerGameTypePacket.h"
#include "mc/util/VarIntDataOutput.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/state/PropertyComponent.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BedrockBlockNames.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/storage/LevelData.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include <uuid.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace playback::functions {

namespace {

constexpr ActorUniqueID  RecordedPlayerUniqueId{std::numeric_limits<int64_t>::max() - 1024};
constexpr ActorRuntimeID RecordedPlayerRuntimeId{uint64_t{1} << 62};

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

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

bool packetMayReferenceRecordedPlayer(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::AddPlayer:
    case MinecraftPacketIds::PlayerList:
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

nlohmann::ordered_json metaToJson(PlaybackMeta const& meta) {
    auto chunks = nlohmann::ordered_json::object();
    for (auto const& [chunkName, chunkMeta] : meta.chunks) {
        chunks[chunkName] = metaToJson(chunkMeta);
    }

    nlohmann::ordered_json json{
        {"name",       meta.name        },
        {"worldName",  meta.worldName   },
        {"duration",   meta.duration    },
        {"totalTicks", meta.totalTicks  },
        {"chunks",     std::move(chunks)}
    };

    if (meta.initialView) {
        auto const& view    = *meta.initialView;
        json["initialView"] = {
            {"x",     view.x    },
            {"y",     view.y    },
            {"z",     view.z    },
            {"yaw",   view.yaw  },
            {"pitch", view.pitch}
        };
    }

    return json;
}

PlaybackMeta metaFromJson(nlohmann::ordered_json const& json) {
    PlaybackMeta meta;
    meta.name       = json.value("name", meta.name);
    meta.worldName  = json.value("worldName", meta.worldName);
    meta.duration   = json.value("duration", meta.duration);
    meta.totalTicks = json.value("totalTicks", meta.totalTicks);

    auto initialViewIt = json.find("initialView");
    if (initialViewIt != json.end()) {
        if (!initialViewIt->is_object()) {
            throw std::invalid_argument("Playback metadata initialView must be an object");
        }

        meta.initialView = PlaybackView{
            initialViewIt->at("x").get<float>(),
            initialViewIt->at("y").get<float>(),
            initialViewIt->at("z").get<float>(),
            initialViewIt->at("yaw").get<float>(),
            initialViewIt->at("pitch").get<float>()
        };
    }

    auto chunksIt = json.find("chunks");
    if (chunksIt == json.end() || chunksIt->is_null()) {
        return meta;
    }

    if (!chunksIt->is_object()) {
        throw std::invalid_argument("Playback metadata chunks must be an object");
    }

    for (auto it = chunksIt->begin(); it != chunksIt->end(); ++it) {
        meta.chunks.insert_or_assign(it.key(), metaFromJson(it.value()));
    }
    return meta;
}

} // namespace

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto j = nlohmann::ordered_json::parse(json);
    return metaFromJson(j);
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
        getLogger().debug("Resume recording");
        return;
    }

    auto& playback = playback::Playback::getInstance();
    if (playback.isReplayMode()) {
        mState = State::Idle;
        getLogger().debug("Skip recording because current save is a replay save");
        return;
    }

    if (mState.load() != State::Idle) {
        getLogger().debug("Recorder is already active");
        return;
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
    State state = mState.exchange(State::Closing);
    if (state == State::Idle) {
        getLogger().debug("Recorder is not active");
        mState = State::Idle;
        return;
    }
    if (state == State::Closing) {
        getLogger().debug("Recorder is already closing");
        return;
    }

    endTick(true);
    if (mState.load() != State::Closing) return;
    logRecordedGamePacketSummary();
    saveRecording();
}

void Recorder::saveRecording() {
    if (!mAsyncReplaySaver) {
        getLogger().error("Failed to stop recording because replay saver is not initialized");
        mState = State::Idle;
        return;
    }

    auto replayPath = mAsyncReplaySaver->finish();
    auto saverError = mAsyncReplaySaver->getError();
    mAsyncReplaySaver.reset();
    mState = State::Idle;

    if (saverError || replayPath.empty()) {
        getLogger().error(
            "Failed to save recording: {}",
            saverError.value_or("the replay saver did not return a completed recording path")
        );
        return;
    }

    auto            replayDir = utils::PathUtils::getReplaysDir();
    std::error_code ec;
    std::filesystem::create_directories(replayDir, ec);
    if (ec) {
        getLogger().error("Error while trying to create replay folder: {}", ec.message());
        return;
    }

    auto outputPath = replayDir / findAvailableReplayName(replayDir, currentReplayTimestampName());
    if (!ReplayExporter::exportReplay(replayPath, outputPath, "")) {
        getLogger().error("Failed to save replay data after recording stopped");
        return;
    }
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
        "UpdateBlockSynced={}, UpdateSubChunkBlocks={}",
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
        count(MinecraftPacketIds::UpdateSubChunkBlocks)
    );
}

void Recorder::resetStateForNewRecording() {
    mAsyncReplaySaver = std::make_unique<AsyncReplaySaver>();

    mMetadata.chunks.clear();
    mMetadata.initialView.reset();
    mRecordingDimension.reset();
    resetChunkSnapshot();
    mOpenChunkView.reset();
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
    mChunkIndex           = 0;
    mTicksInCurrentChunk  = 0;
    mWrittenTicks         = 0;
    mLongestSnapshotStall = {};
    mHasOpenChunk         = false;
    mOpenChunkHasData     = false;
    mNeedsInitialSnapshot = true;
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerArmorPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
        mRecordedGamePacketCounts.clear();
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

    bool dimensionChanged = false;
    if (mRecordingDimension) {
        auto  clientInstance = ll::service::getClientInstance();
        auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
        if (!localPlayer) {
            if (close && mHasOpenChunk) {
                if (!flushGamePackets() || !writeTickBoundary() || !finishCurrentChunk(true)) return;
                return;
            }
            failRecording("The local player is unavailable while recording");
            return;
        }
        dimensionChanged = localPlayer->getDimensionId() != *mRecordingDimension;
    }

    if (dimensionChanged) {
        if (!flushGamePackets() || !writeTickBoundary() || !finishCurrentChunk(close)) return;
        return;
    }

    bool const rotateChunk =
        !close && !mNeedsInitialSnapshot.load() && mHasOpenChunk && mTicksInCurrentChunk + 1 >= RECORD_CHUNK_TICKS;

    if (rotateChunk) {
        auto                                captureStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration barrierWait{};
        bool                                captured = captureChunkSnapshot(barrierWait);
        auto                                elapsed  = std::chrono::steady_clock::now() - captureStart;

        if (!captured) {
            getLogger().error(
                "Replay snapshot capture failed after {:.3f} ms: {}",
                std::chrono::duration<double, std::milli>(elapsed).count(),
                mSnapshotFailure
            );
            failRecording(mSnapshotFailure.empty() ? "Unable to prepare the replay chunk snapshot" : mSnapshotFailure);
            return;
        }

        if (!writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements() || !writeTickBoundary()
            || !finishCurrentChunk(false) || !writeSnapshot() || !commitChunkSnapshot(elapsed, barrierWait)) {
            return;
        }
        return;
    }

    if (!writeInitialSnapshotIfNeeded() || !writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements()
        || !writeTickBoundary()) {
        return;
    }
    if (close && !finishCurrentChunk(true)) return;
}

void Recorder::resetChunkSnapshot() {
    mSnapshotLevelChunks.clear();
    mSnapshotSubChunks.clear();
    mSnapshotEntityPackets.clear();
    mSnapshotView.reset();
    mSnapshotFailure.clear();
}

bool Recorder::captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait) {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        mSnapshotFailure = "The local player is not ready for a chunk snapshot";
        return false;
    }

    auto const dimension = localPlayer->getDimensionId();

    auto mutationGuard = ChunkMutationBarrier::capture();
    barrierWait        = mutationGuard.waited();
    if (!mutationGuard) {
        mSnapshotFailure = "Unable to acquire the chunk mutation barrier";
        return false;
    }

    auto* guardedPlayer = clientInstance->getLocalPlayer();
    if (guardedPlayer != localPlayer || !guardedPlayer || guardedPlayer->getDimensionId() != dimension) {
        mSnapshotFailure = "The player or dimension changed while acquiring the chunk mutation barrier";
        return false;
    }
    auto& dimensionObject = guardedPlayer->getDimension();

    struct SnapshotColumn {
        ChunkPos                    pos;
        std::shared_ptr<LevelChunk> chunk;
    };

    std::vector<SnapshotColumn> columns;
    auto const&                 storage = dimensionObject.getChunkSource().getStorage();
    columns.reserve(storage.size());

    for (auto const& [pos, weakChunk] : storage) {
        auto chunk = weakChunk.lock();
        if (!chunk || chunk->mIsEmptyClientChunk
            || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
            continue;
        }
        columns.push_back(SnapshotColumn{pos, std::move(chunk)});
    }

    if (columns.empty()) {
        mSnapshotFailure = "No loaded level chunks are available for the snapshot";
        return false;
    }

    auto const& position = localPlayer->getPosition();
    auto const& rotation = localPlayer->getRotation();
    auto const  view     = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
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
        return false;
    }

    auto const dimensionMinHeight = static_cast<int>(dimensionObject.mHeightRange->mMin);
    auto const dimensionMaxHeight = static_cast<int>(dimensionObject.mHeightRange->mMax);
    auto const expectedSubChunks  = static_cast<size_t>(dimensionObject.getHeightInSubchunks());
    if (dimensionMinHeight % 16 != 0 || dimensionMaxHeight <= dimensionMinHeight
        || static_cast<size_t>((dimensionMaxHeight - dimensionMinHeight) / 16) != expectedSubChunks) {
        mSnapshotFailure = "The current dimension has an invalid subchunk height range";
        return false;
    }

    // Parallel column serialization
    size_t const       numColumns = columns.size();
    unsigned int const numThreads = std::max(1u, std::thread::hardware_concurrency());
    size_t const       batchSize  = std::max(size_t{1}, (numColumns + numThreads - 1) / numThreads);

    struct ColumnResult {
        std::shared_ptr<LevelChunkPacket> levelChunk;
        std::shared_ptr<SubChunkPacket>   subChunk;
        std::string                       error;
    };

    std::vector<std::future<std::vector<ColumnResult>>> futures;

    for (unsigned int t = 0; t < numThreads; ++t) {
        size_t start = t * batchSize;
        if (start >= numColumns) break;
        size_t end = std::min(start + batchSize, numColumns);

        futures.push_back(std::async(
            std::launch::async,
            [&columns, start, end, dimension, air, expectedSubChunks, dimensionMinHeight](
            ) -> std::vector<ColumnResult> {
                std::vector<ColumnResult> results;
                results.reserve(end - start);

                auto saveContext = SaveContextFactory::createNetworkSaveContext();
                if (!saveContext) {
                    results.push_back({{}, {}, "Unable to create a network SaveContext for block actors"});
                    return results;
                }

                for (size_t ci = start; ci < end; ++ci) {
                    ColumnResult result;
                    auto const&  pos   = columns[ci].pos;
                    auto&        chunk = *columns[ci].chunk;

                    if (chunk.mIsEmptyClientChunk
                        || chunk.mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
                        result.error =
                            snapshotFailure(pos, std::nullopt, "starting column serialization", "chunk unloaded");
                        results.push_back(std::move(result));
                        return results;
                    }

                    auto const& subChunks = *chunk.mSubChunks;
                    if (subChunks.empty()) {
                        result.error = snapshotFailure(pos, std::nullopt, "validating slots", "no subchunk slots");
                        results.push_back(std::move(result));
                        return results;
                    }
                    if (subChunks.size() != expectedSubChunks
                        || subChunks.size() > static_cast<size_t>(std::numeric_limits<schar>::max()) + 1) {
                        result.error = snapshotFailure(
                            pos,
                            std::nullopt,
                            "validating slots",
                            "subchunk count does not cover the complete dimension height in one packet"
                        );
                        results.push_back(std::move(result));
                        return results;
                    }

                    std::string stage = "creating LevelChunkPacket";
                    try {
                        auto levelBase = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
                        if (!levelBase || levelBase->getId() != MinecraftPacketIds::FullChunkData) {
                            result.error =
                                snapshotFailure(pos, std::nullopt, stage, "native packet factory returned wrong type");
                            results.push_back(std::move(result));
                            return results;
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
                            result.error =
                                snapshotFailure(pos, std::nullopt, stage, "native packet factory returned wrong type");
                            results.push_back(std::move(result));
                            return results;
                        }
                        auto subChunkPacket   = std::static_pointer_cast<SubChunkPacket>(std::move(subChunkBase));
                        int  minimumSubChunkY = dimensionMinHeight / 16;

                        subChunkPacket->mCacheEnabled  = false;
                        subChunkPacket->mDimensionType = dimension;
                        subChunkPacket->mCenterPos     = SubChunkPos{pos.x, minimumSubChunkY, pos.z};
                        subChunkPacket->mSubChunkData->clear();
                        subChunkPacket->mSubChunkData->reserve(subChunks.size());

                        for (size_t index = 0; index < subChunks.size(); ++index) {
                            auto const& subChunk = subChunks[index];
                            int absoluteY = static_cast<int>(static_cast<unsigned char>(subChunk.mAbsoluteIndex));
                            stage         = "validating subchunk slot";

                            if (subChunk.isPlaceHolderSubChunk()) continue;
                            if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
                                && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished) {
                                continue;
                            }

                            BinaryStream serializedSubChunk;
                            bool         allAir = subChunk.isUniform(*air);
                            if (!allAir) {
                                VarIntDataOutput output(serializedSubChunk);
                                subChunk.serialize(output, true);
                            }

                            stage = "serializing block actors";
                            {
                                VarIntDataOutput output(serializedSubChunk);
                                chunk.serializeBlockEntitiesForSubChunk(
                                    output,
                                    SubChunkPos{pos.x, absoluteY, pos.z},
                                    *saveContext
                                );
                            }

                            SubChunkPacket::SubChunkPosOffset offset{};
                            offset.mX       = 0;
                            offset.mY       = static_cast<schar>(index);
                            offset.mZ       = 0;
                            auto resultFlag = allAir ? SubChunkPacket::SubChunkRequestResult::SuccessAllAir
                                                     : SubChunkPacket::SubChunkRequestResult::Success;
                            subChunkPacket->mSubChunkData->emplace_back(offset, resultFlag);
                            auto& data               = subChunkPacket->mSubChunkData->back();
                            data.mSerializedSubChunk = std::move(serializedSubChunk.mBuffer);
                            data.mBlobId             = 0;

                            stage = "populating heightmaps";
                            chunk.populateHeightMapDataForSubChunkPacket(static_cast<short>(absoluteY), data);
                        }

                        result.levelChunk = std::move(level);
                        if (!subChunkPacket->mSubChunkData->empty()) {
                            result.subChunk = std::move(subChunkPacket);
                        }
                    } catch (std::exception const& exception) {
                        result.error = snapshotFailure(pos, std::nullopt, stage, exception.what());
                        results.push_back(std::move(result));
                        return results;
                    } catch (...) {
                        result.error = snapshotFailure(pos, std::nullopt, stage, "unknown engine serialization error");
                        results.push_back(std::move(result));
                        return results;
                    }

                    results.push_back(std::move(result));
                }
                return results;
            }
        ));
    }

    // Collect results from all batches
    std::vector<std::shared_ptr<LevelChunkPacket>> levelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>   subChunkPackets;
    levelChunks.reserve(columns.size());
    subChunkPackets.reserve(columns.size());

    for (auto& future : futures) {
        auto batchResults = future.get();
        for (auto& result : batchResults) {
            if (!result.error.empty()) {
                mSnapshotFailure = std::move(result.error);
                return false;
            }
            levelChunks.emplace_back(std::move(result.levelChunk));
            if (result.subChunk) {
                subChunkPackets.emplace_back(std::move(result.subChunk));
            }
        }
    }

    auto* finalPlayer = clientInstance->getLocalPlayer();
    if (finalPlayer != localPlayer || !finalPlayer || finalPlayer->getDimensionId() != dimension) {
        mSnapshotFailure = "The player or dimension changed while capturing the chunk snapshot";
        return false;
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
            return false;
        }
        static_cast<SetTimePacket&>(*timePacket).mTime = level.getTime();
        appendEntityPacket(*timePacket);

        auto                 actors = level.getRuntimeActorList();
        std::vector<Player*> snapshotPlayers;
        snapshotPlayers.reserve(actors.size());

        for (auto* actor : actors) {
            if (!actor || !actor->isAlive() || actor->getDimensionId() != dimension) continue;
            if (actor->isPlayer()) snapshotPlayers.emplace_back(static_cast<Player*>(actor));
        }

        if (!snapshotPlayers.empty()) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerList);
            if (!packet) {
                mSnapshotFailure = "Unable to create the entity snapshot player list packet";
                return false;
            }

            auto&       playerListPacket = static_cast<PlayerListPacket&>(*packet);
            auto const& playerList       = level.getPlayerList();
            playerListPacket.mAction     = PlayerListPacketType::Add;
            playerListPacket.mEntries->reserve(snapshotPlayers.size());
            for (auto const* player : snapshotPlayers) {
                auto            entry = playerList.find(player->getUuid());
                PlayerListEntry snapshotEntry =
                    entry != playerList.end() ? PlayerListEntry(entry->second) : PlayerListEntry(*player);
                if (player == finalPlayer) {
                    snapshotEntry.mId   = *mRecordedLocalPlayerId;
                    snapshotEntry.mUUID = *mRecordedLocalPlayerUuid;
                    snapshotEntry.mXUID->clear();
                    snapshotEntry.mPlatformOnlineId->clear();
                }
                playerListPacket.mEntries->emplace_back(std::move(snapshotEntry));
            }
            appendEntityPacket(playerListPacket);
        }

        for (auto* actor : actors) {
            if (!actor || !actor->isAlive() || actor->getDimensionId() != dimension) continue;

            std::shared_ptr<Packet> packet;
            if (actor == finalPlayer) packet = std::make_shared<AddPlayerPacket>(*finalPlayer);
            else packet = actor->tryCreateAddActorPacket();
            if (!packet) {
                getLogger().debug("Skipping unsupported replay snapshot actor {}", actor->getOrCreateUniqueID().rawID);
                continue;
            }
            if (actor == finalPlayer) {
                auto& addPlayer           = static_cast<AddPlayerPacket&>(*packet);
                addPlayer.mUuid           = *mRecordedLocalPlayerUuid;
                addPlayer.mEntityId       = *mRecordedLocalPlayerId;
                addPlayer.mRuntimeId      = *mRecordedLocalPlayerRuntimeId;
                addPlayer.mPlayerGameType = finalPlayer->getPlayerGameType();
                addPlayer.mPlatformOnlineId->clear();
                addPlayer.mDeviceId->clear();
            }
            appendEntityPacket(*packet);

            auto const runtimeId = actor == finalPlayer ? *mRecordedLocalPlayerRuntimeId : actor->getRuntimeID();
            if (actor->isPlayer()) {
                auto const*        player = static_cast<Player const*>(actor);
                MobEquipmentPacket equipment(
                    runtimeId,
                    player->getSelectedItem(),
                    player->getSelectedItemSlot(),
                    player->getSelectedItemSlot(),
                    ContainerID::Inventory
                );
                appendEntityPacket(equipment);

                MobArmorEquipmentPacket armor(*player);
                if (actor == finalPlayer) armor.mRuntimeId = *mRecordedLocalPlayerRuntimeId;
                appendEntityPacket(armor);
            } else if (actor->hasCategory(ActorCategory::Mob)) {
                MobEquipmentPacket equipment(runtimeId, actor->getCarriedItem(), 0, 0, ContainerID::Inventory);
                appendEntityPacket(equipment);

                MobArmorEquipmentPacket armor(*actor);
                appendEntityPacket(armor);
            }
        }
    } catch (std::exception const& exception) {
        mSnapshotFailure = "Unable to serialize the entity snapshot: " + std::string(exception.what());
        return false;
    } catch (...) {
        mSnapshotFailure = "Unable to serialize the entity snapshot";
        return false;
    }

    mSnapshotLevelChunks   = std::move(levelChunks);
    mSnapshotSubChunks     = std::move(subChunkPackets);
    mSnapshotEntityPackets = std::move(entityPackets);
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mSnapshotDimensionDataPayload = mDimensionDataPayload;
    }
    mSnapshotView = view;
    if (!mMetadata.initialView) {
        mMetadata.initialView = view;
    }
    mRecordingDimension = dimension;

    size_t subChunkCount = 0;
    for (auto const& packet : mSnapshotSubChunks) subChunkCount += packet->mSubChunkData->size();
    getLogger().debug(
        "Prepared replay snapshot with {} columns, {} subchunks, and {} entity packets",
        mSnapshotLevelChunks.size(),
        subChunkCount,
        mSnapshotEntityPackets.size()
    );
    return true;
}

bool Recorder::commitChunkSnapshot(
    std::chrono::steady_clock::duration captureElapsed,
    std::chrono::steady_clock::duration barrierWait
) {
    mOpenChunkView = mSnapshotView;
    mSnapshotView.reset();
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

    auto                                captureStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration barrierWait{};
    bool                                captured = captureChunkSnapshot(barrierWait);
    auto                                elapsed  = std::chrono::steady_clock::now() - captureStart;

    if (!captured) {
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

    if (!writeSnapshot()) return false;
    return commitChunkSnapshot(elapsed, barrierWait);
}

bool Recorder::writeSnapshot() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing a snapshot");
        return false;
    }

    if (!mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.startSnapshot(); })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue the replay snapshot header"));
        return false;
    }

    std::vector<AsyncReplaySaver::GamePacket> gamePackets;
    gamePackets.reserve(
        (mSnapshotDimensionDataPayload.empty() ? 0 : 1) + mSnapshotLevelChunks.size() + mSnapshotSubChunks.size()
        + mSnapshotEntityPackets.size()
    );

    if (!mSnapshotDimensionDataPayload.empty()) {
        gamePackets.emplace_back(PlaybackSerializedGamePacket{
            static_cast<int32_t>(MinecraftPacketIds::DimensionDataPacket),
            mSnapshotDimensionDataPayload
        });
    }

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
    return true;
}

bool Recorder::flushGamePackets() {
    if (!mAsyncReplaySaver) {
        failRecording("Replay saver is not initialized while writing game packets");
        return false;
    }

    std::vector<PlaybackSerializedGamePacket> pending;
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        pending.swap(mPendingGamePackets);
    }

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
        ActorUniqueID id;
        std::string   state;
    };

    std::vector<MovementRecord>                    changed;
    std::unordered_map<ActorUniqueID, std::string> current;
    auto                                           actors = localPlayer->getLevel().getRuntimeActorList();
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

        PlaybackBuffer state;
        state.writeFloat(position.x, nullptr, nullptr);
        state.writeFloat(position.y, nullptr, nullptr);
        state.writeFloat(position.z, nullptr, nullptr);
        state.writeFloat(rotation.x, nullptr, nullptr);
        state.writeFloat(rotation.y, nullptr, nullptr);
        state.writeFloat(headYaw, nullptr, nullptr);
        state.writeFloat(bodyYaw, nullptr, nullptr);
        state.writeBool(actor->isOnGround(), nullptr, nullptr);

        if (actor == localPlayer && !mRecordedLocalPlayerId) {
            failRecording("The recorded local player identity is unavailable while writing entity movements");
            return false;
        }
        auto const id       = actor == localPlayer ? *mRecordedLocalPlayerId : actor->getOrCreateUniqueID();
        auto       it       = current.emplace(id, std::move(state.mBuffer)).first;
        auto       previous = mLastEntityMovements.find(id);
        if (previous == mLastEntityMovements.end() || previous->second != it->second) {
            changed.emplace_back(id, it->second);
        }
    }
    mLastEntityMovements = std::move(current);

    if (changed.empty()) return true;
    if (!mAsyncReplaySaver->submit([changed = std::move(changed)](ReplayWriter& writer) {
            auto& action = ActionMoveEntities::getInstance();
            writer.startAction(action);
            writer.mStream.writeVarInt(0, nullptr, nullptr);
            writer.mStream.writeVarInt(1, nullptr, nullptr);
            writer.mStream.writeVarInt(static_cast<int>(changed.size()), nullptr, nullptr);
            for (auto const& movement : changed) {
                writer.mStream.writeVarInt64(movement.id.rawID, nullptr, nullptr);
                writer.mStream.write(movement.state.data(), movement.state.size());
            }
            writer.finishAction(action);
        })) {
        auto error = mAsyncReplaySaver->getError();
        failRecording(error.value_or("Unable to queue entity movements"));
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
        PlaybackBuffer serialized;
        packet.write(serialized);
        if (previous && *previous == serialized.mBuffer) return;
        previous = serialized.mBuffer;
        recordGamePacket(packet);
    };

    auto&              entityContext = localPlayer->getEntityContext();
    auto*              properties    = entityContext.tryGetComponent<PropertyComponent>().as_ptr();
    SetActorDataPacket actorData(localPlayer->getRuntimeID(), *localPlayer->mEntityData, properties, 0, true);
    recordChanged(actorData, mLastLocalPlayerDataPacket);

    MobEquipmentPacket equipment(
        localPlayer->getRuntimeID(),
        localPlayer->getSelectedItem(),
        localPlayer->getSelectedItemSlot(),
        localPlayer->getSelectedItemSlot(),
        ContainerID::Inventory
    );
    recordChanged(equipment, mLastLocalPlayerEquipmentPacket);

    MobArmorEquipmentPacket armor(*localPlayer);
    recordChanged(armor, mLastLocalPlayerArmorPacket);

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
    return true;
}

void Recorder::recordSpawnedActor(ActorRuntimeID runtimeId) {
    if (!isActive()) return;

    auto  client      = ll::service::getClientInstance();
    auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
    auto* actor       = localPlayer ? localPlayer->getLevel().getRuntimeEntity(runtimeId, false) : nullptr;
    if (!actor) {
        getLogger().warn("Unable to find spawned actor {} for timeline recording", runtimeId.rawID);
        return;
    }

    auto spawnPacket = actor->tryCreateAddActorPacket();
    if (!spawnPacket) {
        getLogger().warn("Unable to create a replayable spawn packet for actor {}", runtimeId.rawID);
        return;
    }
    recordGamePacket(*spawnPacket);

    if (!actor->hasCategory(ActorCategory::Mob)) return;

    MobEquipmentPacket equipment(runtimeId, actor->getCarriedItem(), 0, 0, ContainerID::Inventory);
    recordGamePacket(equipment);

    MobArmorEquipmentPacket armor(*actor);
    recordGamePacket(armor);
}

void Recorder::recordGamePacket(Packet const& packet) {
    auto const packetId          = packet.getId();
    auto const state             = mState.load(std::memory_order_acquire);
    auto const recordingTimeline = state == State::Recording || state == State::Closing;
    if (!recordingTimeline && packetId != MinecraftPacketIds::DimensionDataPacket) return;

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
        if (packetId == MinecraftPacketIds::SubChunkPacket
            && static_cast<bool>(static_cast<SubChunkPacket const&>(packet).mCacheEnabled)) {
            return;
        }

        PlaybackBuffer stream;
        packet.write(stream);

        {
            std::scoped_lock lock(mPendingGamePacketsMutex);
            if (packetId == MinecraftPacketIds::DimensionDataPacket) mDimensionDataPayload = stream.mBuffer;
        }

        if (!recordingTimeline) return;

        if (packetId == MinecraftPacketIds::MoveAbsoluteActor || packetId == MinecraftPacketIds::MovePlayer
            || packetId == MinecraftPacketIds::NetworkChunkPublisherUpdate
            || packetId == MinecraftPacketIds::ChunkRadiusUpdated) {
            return;
        }

        auto  clientInstance = ll::service::getClientInstance();
        auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
        if (localPlayer && mRecordedLocalPlayerId && mRecordedLocalPlayerRuntimeId && mRecordedLocalPlayerUuid
            && packetMayReferenceRecordedPlayer(packetId)) {
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
        for (auto const& [pendingId, pendingPayload] : mPendingGamePackets) {
            if (pendingId == static_cast<int32_t>(packetId) && pendingPayload == stream.mBuffer) return;
        }
        mPendingGamePackets.push_back({static_cast<int32_t>(packetId), std::move(stream.mBuffer)});
        auto& recordedCount = mRecordedGamePacketCounts[static_cast<int32_t>(packetId)];
        if (recordedCount++ == 0) {
            getLogger().debug("Recording timeline packet {} ({})", packet.getName(), static_cast<int32_t>(packetId));
        }
    } catch (std::exception const& exception) {
        getLogger().error("Unable to serialize incoming game packet {}: {}", packet.getName(), exception.what());
    } catch (...) {
        getLogger().error("Unable to serialize incoming game packet {}", packet.getName());
    }
}

bool Recorder::finishCurrentChunk(bool close) {
    if (!mHasOpenChunk || !mOpenChunkHasData) return true;
    if (!mOpenChunkView) {
        failRecording("Replay chunk has no snapshot playback view");
        return false;
    }

    std::string chunkName = "chunk_" + std::to_string(mChunkIndex) + ".bin";

    PlaybackMeta chunkMeta;
    chunkMeta.name        = chunkName;
    chunkMeta.worldName   = mMetadata.worldName;
    chunkMeta.duration    = mTicksInCurrentChunk;
    chunkMeta.initialView = mOpenChunkView;
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
    mTicksInCurrentChunk = 0;
    mHasOpenChunk        = false;
    mOpenChunkHasData    = false;
    mOpenChunkView.reset();

    if (!close) {
        mNeedsInitialSnapshot = true;
    }
    return true;
}

void Recorder::failRecording(std::string_view reason) { cancelRecording(reason); }

void Recorder::cancelRecording(std::string_view reason) {
    getLogger().error("Recording cancelled: {}", reason);
    if (mAsyncReplaySaver) {
        mAsyncReplaySaver->cancel();
        mAsyncReplaySaver.reset();
    }

    mState                = State::Idle;
    mNeedsInitialSnapshot = true;
    resetChunkSnapshot();
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerArmorPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
    }
}

} // namespace playback::functions
