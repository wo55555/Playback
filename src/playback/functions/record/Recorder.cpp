#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/packet/PacketLifecycle.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/shared_types/legacy/LevelEvent.h"
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
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/DimensionDataPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/LevelSoundEventPacket.h"
#include "mc/network/packet/MobArmorEquipmentPacket.h"
#include "mc/network/packet/MobEffectPacket.h"
#include "mc/network/packet/MobEquipmentPacket.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/PlayerListPacketType.h"
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
#include "mc/network/packet/UpdatePlayerGameTypePacket.h"
#include "mc/util/VarIntDataOutput.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/state/PropertyComponent.h"
#include "mc/world/effect/MobEffectInstance.h"
#include "mc/world/item/SaveContextFactory.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/BedrockBlockNames.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
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
#include <future>
#include <iomanip>
#include <iterator>
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

namespace playback::functions {

namespace {

constexpr ActorUniqueID  RecordedPlayerUniqueId{std::numeric_limits<int64_t>::max() - 1024};
constexpr ActorRuntimeID RecordedPlayerRuntimeId{uint64_t{1} << 62};

struct NetworkPacketFilter {
    [[nodiscard]] static constexpr bool shouldFilter(MinecraftPacketIds packetId) {
        switch (packetId) {
        case MinecraftPacketIds::AddActor:
        case MinecraftPacketIds::AddItemActor:
            return true;
        default:
            return false;
        }
    }
};

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

void displayRecordingMessage(char const* message) {
    auto client = ll::service::getClientInstance();
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (player) player->displayClientMessage(message, std::nullopt);
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
        {"name",       meta.name        },
        {"worldName",  meta.worldName   },
        {"totalTicks", meta.totalTicks  },
        {"chunks",     std::move(chunks)}
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

    auto const& chunks = json.at("chunks");
    if (!chunks.is_object()) {
        throw std::invalid_argument("Playback metadata chunks must be an object");
    }

    for (auto it = chunks.begin(); it != chunks.end(); ++it) {
        meta.chunks.insert_or_assign(it.key(), chunkMetaFromJson(it.value()));
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

RecordingStatusSnapshot Recorder::getStatusSnapshot() const {
    auto const state = mState.load(std::memory_order_acquire);
    RecordingState publicState = RecordingState::Idle;
    switch (state) {
    case State::Recording:
        publicState = RecordingState::Recording;
        break;
    case State::Paused:
        publicState = RecordingState::Paused;
        break;
    case State::Closing:
        publicState = RecordingState::Closing;
        break;
    case State::Idle:
    default:
        break;
    }

    auto duration = mRecordedDuration;
    if (state == State::Recording && mRecordingStartedAt.time_since_epoch().count() != 0) {
        duration += std::chrono::steady_clock::now() - mRecordingStartedAt;
    }
    return {publicState, std::chrono::duration_cast<std::chrono::seconds>(duration)};
}

void Recorder::start() {
    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        getLogger().error("Recording requires a local player");
        return;
    }

    if (mState.load() == State::Paused) {
        mRecordingStartedAt = std::chrono::steady_clock::now();
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
    mRecordedDuration    = {};
    mRecordingStartedAt  = std::chrono::steady_clock::now();
    displayRecordingMessage("§6[PlayBack]录制已开始");
    getLogger().info("Recording started");
}

void Recorder::pause() {
    if (mState.load() != State::Recording) return;
    mRecordedDuration += std::chrono::steady_clock::now() - mRecordingStartedAt;
    mRecordingStartedAt = {};
    mState = State::Paused;
}

void Recorder::stop() {
    State state = mState.exchange(State::Closing);
    if (state == State::Idle) {
        mState = State::Idle;
        return;
    }
    if (state == State::Closing) {
        return;
    }

    displayRecordingMessage("§6[PlayBack]录制已结束");

    if (state == State::Recording) {
        mRecordedDuration += std::chrono::steady_clock::now() - mRecordingStartedAt;
        mRecordingStartedAt = {};
    }

    endTick(true);
    if (mState.load() != State::Closing) return;
    if (mNeedsInitialSnapshot.load(std::memory_order_acquire)) {
        auto reason = mSnapshotFailure.empty()
                        ? std::string("Recording stopped before the initial replay snapshot was ready")
                        : "Recording stopped without an initial replay snapshot: " + mSnapshotFailure;
        cancelRecording(reason);
        return;
    }
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

    auto  outputPath        = replayDir / findAvailableReplayName(replayDir, currentReplayTimestampName());
    auto* thumbnailProvider = mThumbnailCaptureProvider.load(std::memory_order_acquire);
    if (!mThumbnailCaptureRequested || !thumbnailProvider
        || !thumbnailProvider->saveReplayThumbnail(replayPath / "icon.png")) {
        getLogger().warn("Unable to save replay thumbnail for {}", replayPath);
    }
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

    mMetadata.chunks.clear();
    mMetadata.totalTicks = 0;
    mRecordingDimension.reset();
    resetChunkSnapshot();
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
    mNeedsInitialSnapshot          = true;
    mDimensionTransitionPending    = false;
    mDimensionTransitionTargetId   = 0;
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
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

    auto  clientInstance   = ll::service::getClientInstance();
    auto* localPlayer      = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    bool  dimensionChanged = false;
    if (mRecordingDimension) {
        if (!localPlayer) {
            if (!close && mDimensionTransitionPending.load(std::memory_order_acquire)) {
                // A dimension transition can temporarily detach the local player. The packets still belong to this
                // client tick and must not be collapsed into a later snapshot boundary.
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

    if (dimensionChanged) {
        auto const currentDimensionId = localPlayer->getDimensionId().id;

        // Finish the client tick in the old chunk before snapshotting the state produced by that tick.
        if (!writeLocalPlayerState() || !flushGamePackets() || !writeEntityMovements() || !writeTickBoundary()
            || !finishCurrentChunk(close)) {
            return;
        }
        if (close) return;

        auto                                captureStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration barrierWait{};
        auto                                captureResult = captureChunkSnapshot(barrierWait);
        auto                                elapsed       = std::chrono::steady_clock::now() - captureStart;

        if (captureResult != SnapshotCaptureResult::Success) {
            getLogger().error(
                "New dimension replay snapshot capture failed after {:.3f} ms: {}",
                std::chrono::duration<double, std::milli>(elapsed).count(),
                mSnapshotFailure
            );
            failRecording(
                mSnapshotFailure.empty() ? "Unable to prepare the new dimension replay snapshot" : mSnapshotFailure
            );
            return;
        }

        mCurrentChunkForcePlaySnapshot = true;
        if (!writeSnapshot() || !commitChunkSnapshot(elapsed, barrierWait)) {
            return;
        }
        bool const reachedRequestedDimension =
            !mDimensionTransitionPending.load(std::memory_order_acquire)
            || currentDimensionId == mDimensionTransitionTargetId.load(std::memory_order_acquire);
        if (reachedRequestedDimension) {
            mDimensionTransitionPending  = false;
            mDimensionTransitionTargetId = 0;
        }
        return;
    }

    bool const rotateChunk =
        !close && !mNeedsInitialSnapshot.load() && mHasOpenChunk && mTicksInCurrentChunk + 1 >= RECORD_CHUNK_TICKS;

    if (rotateChunk) {
        auto                                captureStart = std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration barrierWait{};
        auto                                captureResult = captureChunkSnapshot(barrierWait);
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
    mSnapshotConfigurationPackets.clear();
    mSnapshotEntityPackets.clear();
    mSnapshotLocalPlayerPayload.reset();
    mSnapshotView.reset();
    mSnapshotDimension.reset();
    mSnapshotFailure.clear();
}

Recorder::SnapshotCaptureResult Recorder::captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait) {
    resetChunkSnapshot();

    auto  clientInstance = ll::service::getClientInstance();
    auto* localPlayer    = clientInstance ? clientInstance->getLocalPlayer() : nullptr;
    if (!localPlayer) {
        mSnapshotFailure = "The local player is not ready for a chunk snapshot";
        return SnapshotCaptureResult::NotReady;
    }

    auto const dimension = localPlayer->getDimensionId();

    auto mutationGuard = ChunkMutationBarrier::capture();
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
            [&columns, start, end, dimension, air, dimensionMinHeight]() -> std::vector<ColumnResult> {
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

                            // Request-mode columns contain placeholders for sections the client has not received.
                            // They are not air and are recorded later if the server sends a successful response.
                            if (subChunk.isPlaceHolderSubChunk()) continue;
                            if (subChunk.mSubChunkState != SubChunk::SubChunkState::Normal
                                && subChunk.mSubChunkState != SubChunk::SubChunkState::RequestFinished) {
                                continue;
                            }

                            int const relativeY = actualAbsoluteY - minimumSubChunkY;
                            if (relativeY < std::numeric_limits<schar>::min()
                                || relativeY > std::numeric_limits<schar>::max()) {
                                result.error = snapshotFailure(
                                    pos,
                                    actualAbsoluteY,
                                    stage,
                                    "subchunk offset is outside the packet range"
                                );
                                results.push_back(std::move(result));
                                return results;
                            }

                            BinaryStream serializedSubChunk;
                            bool const   allAir = subChunk.isUniform(*air);
                            if (!allAir) {
                                stage = "serializing subchunk";
                                VarIntDataOutput subChunkOutput(serializedSubChunk);
                                // Persistent block-state palettes are independent of the source world's runtime
                                // network IDs, which can differ for servers with custom blocks such as Hive.
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
                return SnapshotCaptureResult::Failed;
            }
            levelChunks.emplace_back(std::move(result.levelChunk));
            if (result.subChunk) subChunkPackets.emplace_back(std::move(result.subChunk));
        }
    }

    auto* finalPlayer = clientInstance->getLocalPlayer();
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

        auto actors = level.getRuntimeActorList();

        std::vector<Player*> snapshotPlayers{finalPlayer};
        snapshotPlayers.reserve(actors.size() + 1);
        for (auto* actor : actors) {
            if (!actor || actor == finalPlayer || !actor->isAlive() || actor->getDimensionId() != dimension) continue;
            if (actor->isPlayer()) snapshotPlayers.emplace_back(static_cast<Player*>(actor));
        }
        if (!snapshotPlayers.empty()) {
            auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::PlayerList);
            if (!packet) {
                mSnapshotFailure = "Unable to create the entity snapshot player list packet";
                return SnapshotCaptureResult::Failed;
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
                auto const selectedSlot = actor.isPlayer() ? static_cast<Player const&>(actor).getSelectedItemSlot() : 0;
                PlaybackSetEquipmentPacket equipment(actor, runtimeId, selectedSlot);
                for (auto const& packet : equipment.createPackets()) {
                    appendEntityPacket(*packet);
                }
            }
            return true;
        };

        // The recorded local player is required snapshot state, independent of the transient runtime actor list.
        AddPlayerPacket recordedPlayer(*finalPlayer);
        recordedPlayer.mUuid           = *mRecordedLocalPlayerUuid;
        recordedPlayer.mEntityId       = *mRecordedLocalPlayerId;
        recordedPlayer.mRuntimeId      = *mRecordedLocalPlayerRuntimeId;
        recordedPlayer.mPlayerGameType = finalPlayer->getPlayerGameType();
        recordedPlayer.mPlatformOnlineId->clear();
        recordedPlayer.mDeviceId->clear();
        PlaybackBuffer recordedPlayerStream;
        recordedPlayer.write(recordedPlayerStream);
        mSnapshotLocalPlayerPayload = std::move(recordedPlayerStream.mBuffer);
        if (!appendActorState(*finalPlayer, *mRecordedLocalPlayerRuntimeId)) {
            return SnapshotCaptureResult::Failed;
        }

        for (auto* actor : actors) {
            if (!actor || actor == finalPlayer || !actor->isAlive() || actor->getDimensionId() != dimension) continue;

            auto packet = actor->tryCreateAddActorPacket();
            if (!packet) continue;
            appendEntityPacket(*packet);
            if (!appendActorState(*actor, actor->getRuntimeID())) return SnapshotCaptureResult::Failed;
        }

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
    mSnapshotView       = view;
    mSnapshotDimension  = SnapshotDimension{dimension, dimensionMinHeight, dimensionMaxHeight};
    mRecordingDimension = dimension;

    return SnapshotCaptureResult::Success;
}

bool Recorder::commitChunkSnapshot(
    std::chrono::steady_clock::duration captureElapsed,
    std::chrono::steady_clock::duration barrierWait
) {
    mSnapshotView.reset();
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

    auto                                captureStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration barrierWait{};
    auto                                captureResult = captureChunkSnapshot(barrierWait);
    auto                                elapsed       = std::chrono::steady_clock::now() - captureStart;

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

    if (!writeSnapshot()) return false;
    return commitChunkSnapshot(elapsed, barrierWait);
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
    if (!mSnapshotView || !mSnapshotDimension) {
        failRecording("Replay snapshot context is unavailable");
        return false;
    }

    PlaybackSnapshotContext context;
    context.dimensionId        = mSnapshotDimension->id.id;
    context.dimensionMinHeight = mSnapshotDimension->minHeight;
    context.dimensionMaxHeight = mSnapshotDimension->maxHeight;
    context.x                  = mSnapshotView->x;
    context.y                  = mSnapshotView->y;
    context.z                  = mSnapshotView->z;
    context.yaw                = mSnapshotView->yaw;
    context.pitch              = mSnapshotView->pitch;
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

    PlaybackSetEquipmentPacket equipment(
        *localPlayer,
        localPlayer->getRuntimeID(),
        localPlayer->getSelectedItemSlot()
    );
    auto const* previousEquipment = mLastLocalPlayerEquipmentPacket ? &*mLastLocalPlayerEquipmentPacket : nullptr;
    auto        equipmentPackets = equipment.createPackets(previousEquipment);
    mLastLocalPlayerEquipmentPacket = std::move(equipment);
    for (auto const& packet : equipmentPackets) {
        recordGamePacket(*packet);
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
    return true;
}

void Recorder::recordNetworkGamePacket(Packet const& packet) {
    if (NetworkPacketFilter::shouldFilter(packet.getId())) return;
    recordGamePacket(packet);
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

void Recorder::recordConfigurationPacket(Packet const& packet, PacketLifecycleSemantics const& semantics) {
    try {
        if (!semantics.isConfiguration()) return;

        auto const packetType = packet.getId();

        PlaybackBuffer stream;
        packet.write(stream);

        auto const packetId = static_cast<int32_t>(packetType);
        std::scoped_lock lock(mPendingGamePacketsMutex);

        if (semantics.startsConfigurationEpoch) {
            mConfigurationPackets.clear();
            mConfigurationPacketIndices.clear();
        }

        if (semantics.supersedes) {
            auto const supersededPacketId = static_cast<int32_t>(*semantics.supersedes);
            mConfigurationPackets.erase(
                std::remove_if(
                    mConfigurationPackets.begin(),
                    mConfigurationPackets.end(),
                    [supersededPacketId](auto const& cached) {
                        return cached.mPacketId == supersededPacketId;
                    }
                ),
                mConfigurationPackets.end()
            );
            mConfigurationPacketIndices.clear();
            for (size_t index = 0; index < mConfigurationPackets.size(); ++index) {
                mConfigurationPacketIndices.insert_or_assign(mConfigurationPackets[index].mPacketId, index);
            }
        }

        auto const       existing = mConfigurationPacketIndices.find(packetId);
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

void Recorder::recordGamePacket(Packet const& packet) {
    auto const packetId  = packet.getId();
    auto const semantics = describePacketLifecycle(packetId);
    switch (semantics.lifecycle) {
    case PacketLifecycle::Ignore:
        return;
    case PacketLifecycle::PreWorldHandshake:
    case PacketLifecycle::SnapshotLatest:
    case PacketLifecycle::SnapshotSequence:
        recordConfigurationPacket(packet, semantics);
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
            auto filteredPacket = static_cast<SubChunkPacket const&>(packet);
            if (static_cast<bool>(filteredPacket.mCacheEnabled)) return;

            std::vector<SubChunkPacket::SubChunkPacketData> successfulEntries;
            successfulEntries.reserve(filteredPacket.mSubChunkData->size());
            for (auto const& entry : *filteredPacket.mSubChunkData) {
                if (isSuccessfulSubChunkResult(static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult)
                    )) {
                    successfulEntries.emplace_back(entry);
                }
            }
            *filteredPacket.mSubChunkData = std::move(successfulEntries);
            if (filteredPacket.mSubChunkData->empty()) return;
            filteredPacket.write(stream);
        } else {
            packet.write(stream);
        }

        if (packetId == MinecraftPacketIds::ChangeDimension) {
            auto const& change = static_cast<ChangeDimensionPacket const&>(packet);
            mDimensionTransitionTargetId.store(change.mDimensionId->id, std::memory_order_release);
            mDimensionTransitionPending.store(true, std::memory_order_release);
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
    if (mAsyncReplaySaver) {
        mAsyncReplaySaver->cancel();
        mAsyncReplaySaver.reset();
    }

    mState                       = State::Idle;
    mRecordingStartedAt          = {};
    mRecordedDuration            = {};
    mNeedsInitialSnapshot        = true;
    mDimensionTransitionPending  = false;
    mDimensionTransitionTargetId = 0;
    resetChunkSnapshot();
    mLastEntityMovements.clear();
    mRecordedLocalPlayerId.reset();
    mRecordedLocalPlayerRuntimeId.reset();
    mRecordedLocalPlayerUuid.reset();
    mLastLocalPlayerDataPacket.reset();
    mLastLocalPlayerEquipmentPacket.reset();
    mLastLocalPlayerSwingTime.reset();
    {
        std::scoped_lock lock(mPendingGamePacketsMutex);
        mPendingGamePackets.clear();
    }
}

} // namespace playback::functions
