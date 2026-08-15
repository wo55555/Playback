#include "ReplaySession.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/packet/PacketLifecycle.h"
#include "playback/functions/render/ReplayEntityInterpolator.h"

#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/strict/StrictEntityContext.h"
#include "mc/deps/vanilla_components/OnGroundFlagComponent.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/ActorRotationComponent.h"
#include "mc/entity/components/LocalPlayerDimensionWaitComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "mc/network/IPacketHandlerDispatcher.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/PacketSender.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/AddItemActorPacket.h"
#include "mc/network/packet/AddPaintingPacket.h"
#include "mc/network/packet/AddPlayerPacket.h"
#include "mc/network/packet/BlockActorDataPacket.h"
#include "mc/network/packet/BlockEventPacket.h"
#include "mc/network/packet/ChangeDimensionPacket.h"
#include "mc/network/packet/DimensionDataPacket.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/MovePlayerPacket.h"
#include "mc/network/packet/PackInfoData.h"
#include "mc/network/packet/PlayerActionPacket.h"
#include "mc/network/packet/PlayerActionType.h"
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/RemoveObjectivePacket.h"
#include "mc/network/packet/ResourcePackStackPacket.h"
#include "mc/network/packet/ResourcePacksInfoPacket.h"
#include "mc/network/packet/SetDisplayObjectivePacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/server/NetworkChunkPublisher.h"
#include "mc/util/VarIntDataInput.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/BuiltInActorComponents.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include "mc/world/level/ActorRuntimeIDManager.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/LevelSettings.h"
#include "mc/world/level/chunk/ChunkSource.h"
#include "mc/world/level/chunk/ChunkViewSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/dimension/DimensionArguments.h"
#include "mc/world/level/storage/ILevelListCache.h"


#include "snappy.h"
#include "uuid.h"
#include "zip.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace playback::functions {

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr std::string_view ReplayLevelIdPrefix        = "__playback_replay_world__";
constexpr auto             CenterChunkInjectionBudget = std::chrono::milliseconds(8);
constexpr auto             OuterChunkInjectionBudget  = std::chrono::milliseconds(4);
constexpr auto             SnapshotGamePacketBudget   = std::chrono::milliseconds(2);
constexpr int              SeekTicksPerClientTick     = 400;
constexpr std::array       PlaybackSpeeds{0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f};

bool shouldIgnoreReplayPacket(MinecraftPacketIds packetId) {
    switch (packetId) {
    case MinecraftPacketIds::ContainerOpen:
    case MinecraftPacketIds::ContainerClose:
    case MinecraftPacketIds::ClientboundCloseScreen:
    case MinecraftPacketIds::NetworkChunkPublisherUpdate:
    case MinecraftPacketIds::ChunkRadiusUpdated:
        return true;
    default:
        return false;
    }
}

bool isSuccessfulSubChunkResult(SubChunkPacket::SubChunkRequestResult result) {
    return result == SubChunkPacket::SubChunkRequestResult::Success
        || result == SubChunkPacket::SubChunkRequestResult::SuccessAllAir;
}

std::optional<render::EntityRenderKey> replayEntityRenderKey(Level const& level, ActorUniqueID id) {
    auto const context = level.fetchStrictEntity(id, false);
    auto const entity  = *context.mEntity;
    if (entity.isNull()) return std::nullopt;
    return render::EntityRenderKey{context.mRegistryId, entity.mRawId};
}

render::EntityRenderPosition replayRenderPosition(Vec3 const& position) { return {position.x, position.y, position.z}; }

render::EntityRenderPose replayRenderPose(Actor const& actor) {
    auto const& context  = actor.getEntityContext();
    auto const  rotation = actor.getRotation();

    float headYaw = rotation.y;
    if (auto const headRotation = context.tryGetComponent<ActorHeadRotationComponent>()) {
        headYaw = headRotation->mYHeadRot;
    }

    float bodyYaw = rotation.y;
    if (auto const bodyRotation = context.tryGetComponent<MobBodyRotationComponent>()) {
        bodyYaw = bodyRotation->mYBodyRot;
    }

    return {replayRenderPosition(actor.getPosition()), rotation.x, rotation.y, headYaw, bodyYaw};
}

void cancelNativeMovementInterpolation(Actor& actor, Vec3 const& position, Vec2 const& rotation, float headYaw) {
    auto const interpolator = actor.getEntityContext().tryGetComponent<MovementInterpolatorComponent>();
    if (!interpolator) return;

    *interpolator->mPos          = position;
    *interpolator->mRot          = rotation;
    interpolator->mHeadYaw       = headYaw;
    interpolator->mPositionSteps = 0;
    interpolator->mRotationSteps = 0;
    interpolator->mHeadYawSteps  = 0;
}

std::string createReplayLevelId() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return std::string(ReplayLevelIdPrefix) + uuids::to_string(id);
}

bool isValidReplayLevelId(std::string_view levelId) {
    if (!levelId.starts_with(ReplayLevelIdPrefix)) return false;

    auto uuidText = levelId.substr(ReplayLevelIdPrefix.size());
    auto uuid     = uuids::uuid::from_string(uuidText);
    return uuid && uuids::to_string(*uuid) == uuidText;
}

std::optional<std::string> readArchiveEntry(zip_t* archive, std::string const& name) {
    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat(archive, name.c_str(), 0, &stat) != 0) return std::nullopt;

    auto* file = zip_fopen(archive, name.c_str(), 0);
    if (!file) return std::nullopt;

    std::string data(static_cast<size_t>(stat.size), '\0');
    size_t      offset = 0;
    while (offset < data.size()) {
        auto read = zip_fread(file, data.data() + offset, data.size() - offset);
        if (read <= 0) {
            zip_fclose(file);
            return std::nullopt;
        }
        offset += static_cast<size_t>(read);
    }

    zip_fclose(file);
    return data;
}

bool appendChunkCache(std::string const& compressed, std::vector<std::string>& packets) {
    std::string data;
    if (!snappy::Uncompress(compressed.data(), compressed.size(), &data)) return false;

    ReadOnlyBinaryStream stream(data, false);
    while (stream.mReadPointer < data.size()) {
        if (data.size() - stream.mReadPointer < sizeof(uint32_t)) return false;

        uint32_t size = stream.getUnsignedInt().value();
        if (size > data.size() - stream.mReadPointer) return false;

        packets.emplace_back(data.data() + stream.mReadPointer, size);
        stream.mReadPointer += size;
    }
    return true;
}

struct InjectionReset {
    std::atomic<Packet const*>& injecting;
    ~InjectionReset() { injecting.store(nullptr, std::memory_order_release); }
};

float decodeRotationByte(uchar value) { return static_cast<schar>(value) * (360.0f / 256.0f); }

} // namespace

ReplaySession::ReplaySession()  = default;
ReplaySession::~ReplaySession() = default;

bool ReplaySession::start(std::filesystem::path filePath) {
    if (mActive || mCleanupState != CleanupState::None || !mReplayLevelId.empty()) {
        getLogger().error("Unable to start replay while another replay world is active or being removed");
        return false;
    }
    auto client = ll::service::getClientInstance();
    if (!client || ll::service::getMultiPlayerLevel() || client->hasLevel() || client->isWorldActive()
        || !client->isLeaveGameDone()) {
        getLogger().error("Replay can only be started from the main menu");
        return false;
    }
    auto& game = client->getMinecraftGame_DEPRECATED();
    if (game.isInServer() || game.getServerInstance()) {
        getLogger().error("Replay cannot start until the current world server has completely stopped");
        return false;
    }

    auto screenModel = mScreenModel.lock();
    if (!screenModel) {
        getLogger().error("Unable to start replay because the main menu is not ready");
        return false;
    }

    try {
        if (!init(std::move(filePath))) {
            stop();
            return false;
        }
        if (mSnapshotContexts.empty()) throw std::runtime_error("Replay contains no snapshot contexts");
        if (!prepareReplayResourcePacks(mReaders.front()->readConfigurationPackets())) {
            throw std::runtime_error("Unable to prepare the recorded resource-pack configuration");
        }
        auto const& context = mSnapshotContexts.front();

        LevelSettings settings;
        settings.mGameType                  = GameType::Spectator;
        settings.mForceGameType             = true;
        settings.mGenerator                 = GeneratorType::Void;
        settings.mImmutableWorld            = true;
        settings.mMultiplayerGameIntent     = false;
        settings.mLANBroadcastIntent        = false;
        settings.mDisablePlayerInteractions = true;
        settings.mDefaultSpawn              = BlockPos(Vec3{context.x, context.y, context.z});

        mReplayLevelId            = createReplayLevelId();
        auto dimensionProfile     = std::make_shared<ReplayDimensionProfile>();
        dimensionProfile->levelId = mReplayLevelId;
        for (auto const& snapshot : mSnapshotContexts) {
            auto const minimum = snapshot.dimensionMinHeight;
            auto const maximum = snapshot.dimensionMaxHeight;
            auto const height  = static_cast<int64_t>(maximum) - static_cast<int64_t>(minimum);
            if (minimum < std::numeric_limits<short>::min() || maximum > std::numeric_limits<short>::max()
                || minimum % 16 != 0 || maximum <= minimum || height % 16 != 0) {
                throw std::runtime_error(
                    std::format(
                        "Replay dimension {} has invalid recorded height range [{}, {})",
                        snapshot.dimensionId,
                        minimum,
                        maximum
                    )
                );
            }

            RecordedDimensionHeightRange const range{minimum, maximum};
            auto const [it, inserted] = dimensionProfile->heightRanges.emplace(snapshot.dimensionId, range);
            if (!inserted && it->second != range) {
                throw std::runtime_error(
                    std::format(
                        "Replay dimension {} has conflicting recorded height ranges [{}, {}) and [{}, {})",
                        snapshot.dimensionId,
                        it->second.minimum,
                        it->second.maximum,
                        minimum,
                        maximum
                    )
                );
            }
        }
        mReplayDimensionProfile.store(std::move(dimensionProfile), std::memory_order_release);

        mCleanupState = CleanupState::None;
        mActive       = true;
        screenModel->startLocalServerAsync(mReplayLevelId, "Playback Replay", settings);
        getLogger().info("Starting replay from {} in {}", mReplayFilePath, mReplayLevelId);
        return true;
    } catch (std::exception const& e) {
        getLogger().error("Unable to start replay: {}", e.what());
        stop();
        return false;
    }
}

void ReplaySession::clearReplayData() {
    mReplayDimensionProfile.store({}, std::memory_order_release);
    mStopRequested.store(false, std::memory_order_release);
    mRequestedSeekTick.store(-1, std::memory_order_release);
    mActive       = false;
    mIsPaused     = false;
    mWorldReady   = false;
    mReplayFailed = false;
    mInjectingPacket.store(nullptr, std::memory_order_release);
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mReplayDimension.store(nullptr, std::memory_order_release);
    mInitialSnapshotApplied  = false;
    mChunkInjectionPending   = false;
    mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    mIsProcessingSnapshot    = false;
    mCurrentTick             = 0;
    mReaderIndex             = 0;
    mSeekTargetTick          = -1;
    mExportSeekRequested     = false;
    mSnapMovementDuringSeek  = false;
    mExportTimelinePhase     = ReplayExportTimelinePhase::Inactive;
    mExportTargetTick        = -1;
    mExportCameraViewpoint.reset();
    mPlaybackSpeed           = 1.0f;
    mPlaybackTickAccumulator = 0.0f;
    mReplayTime.reset();
    mPendingReplayDimension.reset();
    mPendingSnapshotApply.reset();
    if (mDimensionTransitionRequest) {
        mDimensionTransitionRequest->status.store(DimensionTransitionStatus::Cancelled, std::memory_order_release);
        mDimensionTransitionRequest.reset();
    }
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;

    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionStartedAt      = {};

    mCompletedDimensionGeneration  = 0;
    mDimensionTransitionGeneration = 0;

    mInjectedLevelChunks     = 0;
    mInjectedSubChunkPackets = 0;
    mInjectedSubChunkEntries = 0;
    mReusedSnapshotColumns   = 0;
    mDirectLevelChunks       = 0;
    mDirectSubChunkPackets   = 0;
    mDirectSubChunkEntries   = 0;
    mReplayPlayer            = nullptr;
    mNetworkHandler          = nullptr;
    mReplayFilePath.clear();
    mMeta = PlaybackMeta{};
    mReaders.clear();
    mSnapshotContexts.clear();
    mChunkPackets.clear();
    mInlineLevelChunkPacketIndices.clear();
    mInlineSubChunkPacketIndices.clear();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
    mPendingLevelChunkIndices.clear();
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mPendingSnapshotLocalPlayer.reset();
    mPendingSnapshotGamePackets.clear();
    mAppliedConfigurationPackets.clear();
    mRecordedEntityIds.clear();
    mEntityRenderKeys.clear();
    render::clearReplayEntityPoses();
    mReplayObjectiveNames.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mApplyingChunkSnapshot      = false;
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionStartedAt    = {};
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
}

void ReplaySession::finishWorldCleanup() {
    releaseReplayResourcePacks();
    clearReplayData();
    mReplayWorldJoined = false;
    mCleanupState      = CleanupState::None;
    mCleanupWaitTicks  = 0;
    mReplayLevelId.clear();
    mOrphanReplayWorldsScanned = false;
}

void ReplaySession::stop() {
    if (mReplayLevelId.empty()) {
        finishWorldCleanup();
        return;
    }
    if (mCleanupState != CleanupState::None) return;

    if (auto level = ll::service::getMultiPlayerLevel(); level && !isReplayLevel(level.value())) {
        getLogger().error("Cancelling replay without leaving the non-replay world {}", level->getLevelId());
        mCleanupState     = CleanupState::ReadyToDelete;
        mCleanupWaitTicks = 0;
        clearReplayData();
        mReplayWorldJoined = false;
        return;
    }

    mCleanupState     = CleanupState::WaitingForExit;
    mCleanupWaitTicks = 0;
    clearReplayData();

    auto client = ll::service::getClientInstance();
    if (!client) {
        getLogger().error("Unable to leave replay world {} because the client is unavailable", mReplayLevelId);
        return;
    }

    getLogger().debug("Leaving replay world {}", mReplayLevelId);
    client->requestLeaveGameAsync();
}

bool ReplaySession::setPaused(bool paused) {
    if (!mActive) return false;
    if (mIsPaused == paused) return true;

    mIsPaused = paused;
    getLogger().debug("Replay {} at tick {}", paused ? "paused" : "playing", mCurrentTick);
    return true;
}

std::optional<render::ReplaySampleTime> ReplaySession::getRenderSampleTime(float partialTick) const noexcept {
    if (!mActive || !mReplayWorldJoined) return std::nullopt;

    auto const appliedTick = std::max(0, mCurrentTick);
    if (mIsPaused) return render::ReplaySampleTime::fromRational(appliedTick, 1);

    return render::ReplaySampleTime::fromPreview(std::max(0, appliedTick - 1), partialTick);
}

std::optional<render::ReplaySampleTime> ReplaySession::getCameraRenderSampleTime(float partialTick) const noexcept {
    if (!mActive || !mReplayWorldJoined) return std::nullopt;

    auto const appliedTick = std::max(0, mCurrentTick);
    if (mIsPaused) return render::ReplaySampleTime::fromRational(appliedTick, 1);
    return render::ReplaySampleTime::fromPreview(appliedTick, partialTick);
}

std::optional<long double> ReplaySession::getFractionalReplayTick(float partialTick) const noexcept {
    auto const sample = getRenderSampleTime(partialTick);
    return sample ? std::optional<long double>{sample->value()} : std::nullopt;
}

bool ReplaySession::beginExportTimeline(int startTick) {
    if (!mActive || mReplayFailed || mExportTimelinePhase != ReplayExportTimelinePhase::Inactive) return false;

    startTick                = std::clamp(startTick, 0, getTotalTicks());
    mIsPaused                = true;
    mPlaybackTickAccumulator = 0.0f;

    mRequestedSeekTick.store(startTick, std::memory_order_release);
    mSeekTargetTick         = -1;
    mExportSeekRequested    = true;
    mSnapMovementDuringSeek = false;
    mExportTargetTick       = startTick;
    mExportTimelinePhase    = ReplayExportTimelinePhase::Initializing;
    getLogger().info("Export timeline initialization queued (startTick={}, currentTick={})", startTick, mCurrentTick);
    return true;
}

void ReplaySession::setExportCameraViewpoint(std::optional<ReplayCameraViewpoint> viewpoint) noexcept {
    if (viewpoint && (!std::isfinite(viewpoint->x) || !std::isfinite(viewpoint->y) || !std::isfinite(viewpoint->z))) {
        viewpoint.reset();
    }
    mExportCameraViewpoint = std::move(viewpoint);
}

ReplaySceneReadiness ReplaySession::getSceneReadiness() const {
    ReplaySceneReadiness result;
    auto const*          player = mReplayPlayer;
    if (!player) return result;

    auto const&           playerPosition = player->getPosition();
    ReplayCameraViewpoint position{playerPosition.x, playerPosition.y, playerPosition.z};
    if (mExportCameraViewpoint) position = *mExportCameraViewpoint;
    if (!std::isfinite(position.x) || !std::isfinite(position.z)) return result;

    ChunkPos const chunkPos{position.x, position.z};
    result.chunkX                     = chunkPos.x;
    result.chunkZ                     = chunkPos.z;
    result.dimensionTransitionPending = mPendingReplayDimension.has_value();
    result.snapshotPending            = mPendingSnapshotApply.has_value() || mApplyingChunkSnapshot;
    result.chunkInjectionPending      = mChunkInjectionPending;

    auto const* dimension = mReplayDimension.load(std::memory_order_acquire);
    result.replayReady    = mActive && mReplayWorldJoined && mWorldReady && !mReplayFailed && dimension;
    if (!result.replayReady) return result;

    constexpr int CameraChunkRadius = 2;
    auto&         chunkSource       = dimension->getChunkSource();
    for (int dz = -CameraChunkRadius; dz <= CameraChunkRadius; ++dz) {
        for (int dx = -CameraChunkRadius; dx <= CameraChunkRadius; ++dx) {
            ChunkPos const candidate{chunkPos.x + dx, chunkPos.z + dz};
            ++result.requiredChunkCount;
            bool const recorded = mSnapshotChunks.contains(candidate) || mApplyingSnapshotChunks.contains(candidate);
            if (recorded) ++result.recordedChunkCount;

            auto const chunk     = chunkSource.getExistingChunk(candidate);
            bool const present   = static_cast<bool>(chunk);
            auto const loadState = chunk ? chunk->mLoadState->load(std::memory_order_acquire) : ChunkState::Unloaded;
            bool const empty     = chunk && chunk->mIsEmptyClientChunk;
            bool const loaded    = chunk && loadState == ChunkState::Loaded;
            if (present) ++result.presentChunkCount;
            if (empty) ++result.emptyChunkCount;
            if (recorded && present && !empty && loaded) ++result.readyChunkCount;

            if (dx == 0 && dz == 0) {
                result.chunkPresent   = present;
                result.chunkEmpty     = empty;
                result.chunkLoadState = static_cast<int>(loadState);
                result.chunkLoaded    = loaded;
            }
        }
    }
    return result;
}

std::unique_ptr<render::ScopedReplayEntityPose>
ReplaySession::createReplayEntityRenderScope(render::ReplaySampleTime const& sample) {
    if (!sample.isValid() || !mActive || !mReplayWorldJoined || !mWorldReady || !refreshReplayPlayer()) return {};

    std::vector<render::EntityRenderTarget> targets;
    targets.reserve(mEntityRenderKeys.size());
    for (auto const& [id, key] : mEntityRenderKeys) {
        targets.push_back({key, mReplayPlayer->getLevel().fetchEntity(id, false)});
    }
    return render::createReplayEntityRenderScope(targets, sample);
}

ReplayExportTickState ReplaySession::prepareExportTick(int targetTick) {
    if (!mActive) return ReplayExportTickState::Unavailable;
    if (mReplayFailed) return ReplayExportTickState::Failed;
    if (mExportTimelinePhase == ReplayExportTimelinePhase::Inactive) return ReplayExportTickState::Unavailable;

    targetTick               = std::clamp(targetTick, 0, getTotalTicks());
    mIsPaused                = true;
    mPlaybackTickAccumulator = 0.0f;

    if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) {
        if (targetTick != mExportTargetTick) return ReplayExportTickState::Invalid;
    } else {
        if (targetTick < mExportTargetTick || targetTick < mCurrentTick) return ReplayExportTickState::Invalid;
        mExportTargetTick = targetTick;

        if (mRequestedSeekTick.load(std::memory_order_acquire) >= 0 || mSeekTargetTick >= 0) {
            getLogger().error("A normal replay seek was requested while continuous export was active");
            return ReplayExportTickState::Failed;
        }
    }

    if (!mReplayWorldJoined || !mWorldReady || !mNetworkHandler) return ReplayExportTickState::Waiting;

    if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) {
        auto const requestedTick = mRequestedSeekTick.load(std::memory_order_acquire);
        if (requestedTick >= 0) {
            mExportSeekRequested = true;
            if (requestedTick != targetTick) mRequestedSeekTick.store(targetTick, std::memory_order_release);
            return ReplayExportTickState::Waiting;
        }
        if (mSeekTargetTick >= 0 || mPendingReplayDimension || mPendingSnapshotApply || mChunkInjectionPending) {
            return ReplayExportTickState::Waiting;
        }
    } else if (mPendingReplayDimension || mPendingSnapshotApply || mChunkInjectionPending) {
        return ReplayExportTickState::Waiting;
    }

    if (mCurrentTick == targetTick) return ReplayExportTickState::Ready;
    if (mReaderIndex >= mReaders.size() && mCurrentTick < targetTick) return ReplayExportTickState::Failed;

    if (mExportTimelinePhase == ReplayExportTimelinePhase::Initializing) {
        mExportSeekRequested = true;
        mRequestedSeekTick.store(targetTick, std::memory_order_release);
    }
    return ReplayExportTickState::Waiting;
}

bool ReplaySession::finishExportTimelineInitialization() {
    if (mExportTimelinePhase != ReplayExportTimelinePhase::Initializing || !mActive || mReplayFailed) return false;
    if (mCurrentTick != mExportTargetTick || mSeekTargetTick >= 0
        || mRequestedSeekTick.load(std::memory_order_acquire) >= 0 || mPendingReplayDimension || mPendingSnapshotApply
        || mChunkInjectionPending) {
        return false;
    }

    mExportSeekRequested    = false;
    mSnapMovementDuringSeek = false;
    mExportTimelinePhase    = ReplayExportTimelinePhase::Continuous;
    return true;
}

void ReplaySession::endExportTimeline() {
    mRequestedSeekTick.store(-1, std::memory_order_release);
    mSeekTargetTick         = -1;
    mExportSeekRequested    = false;
    mSnapMovementDuringSeek = false;
    mExportTargetTick       = -1;
    mExportTimelinePhase    = ReplayExportTimelinePhase::Inactive;
    mExportCameraViewpoint.reset();
}

int ReplaySession::getTotalTicks() const { return std::max(0, mMeta.totalTicks); }

void ReplaySession::adjustPlaybackSpeed(int direction) {
    if (!mActive || direction == 0) return;

    size_t currentIndex = 0;
    for (size_t index = 1; index < PlaybackSpeeds.size(); ++index) {
        if (std::abs(PlaybackSpeeds[index] - mPlaybackSpeed)
            < std::abs(PlaybackSpeeds[currentIndex] - mPlaybackSpeed)) {
            currentIndex = index;
        }
    }

    auto const nextIndex     = static_cast<size_t>(std::clamp(
        static_cast<int>(currentIndex) + (direction < 0 ? -1 : 1),
        0,
        static_cast<int>(PlaybackSpeeds.size() - 1)
    ));
    mPlaybackSpeed           = PlaybackSpeeds[nextIndex];
    mPlaybackTickAccumulator = 0.0f;
    getLogger().debug("Replay speed set to {:.2f}x", mPlaybackSpeed);
}

void ReplaySession::beginSeek(int targetTick) {
    targetTick = std::clamp(targetTick, 0, getTotalTicks());
    if (mReaders.empty()) return;
    if (!refreshReplayPlayer()) throw std::runtime_error("Replay player is unavailable while seeking");
    mEntityRenderKeys.clear();
    render::clearReplayEntityPoses();

    size_t selectedReader = mReaders.size() - 1;
    int    selectedStart  = 0;
    int    chunkStart     = 0;
    size_t chunkIndex     = 0;
    for (auto const& [_, chunkMeta] : mMeta.chunks) {
        int const chunkEnd = chunkStart + std::max(0, chunkMeta.duration);
        if (targetTick < chunkEnd || chunkIndex + 1 == mReaders.size()) {
            selectedReader = chunkIndex;
            selectedStart  = chunkStart;
            break;
        }
        chunkStart = chunkEnd;
        ++chunkIndex;
    }

    bool const exportSeek    = mExportSeekRequested;
    mIsPaused                = true;
    mPlaybackTickAccumulator = 0.0f;
    mSeekTargetTick          = targetTick;
    mSnapMovementDuringSeek  = !exportSeek || mExportTimelinePhase == ReplayExportTimelinePhase::Initializing;
    if (selectedReader >= mSnapshotContexts.size()) {
        throw std::runtime_error("Replay seek snapshot context index is out of range");
    }

    auto const currentDimension      = mReplayPlayer->getDimensionId();
    auto const targetDimension       = DimensionType{mSnapshotContexts[selectedReader].dimensionId};
    bool const changesDimension      = currentDimension != targetDimension;
    bool       crossesForcedSnapshot = false;
    if (selectedReader != mReaderIndex) {
        size_t const firstReader = std::min(selectedReader, mReaderIndex);
        size_t const lastReader  = std::max(selectedReader, mReaderIndex);
        size_t       readerIndex = 0;
        for (auto const& [_, chunkMeta] : mMeta.chunks) {
            if (readerIndex > firstReader && readerIndex <= lastReader && chunkMeta.forcePlaySnapshot) {
                crossesForcedSnapshot = true;
                break;
            }
            ++readerIndex;
        }
    }
    bool const followRecordedPlayer = changesDimension || crossesForcedSnapshot;
    bool const forceExportSnapshot  = exportSeek && mExportTimelinePhase == ReplayExportTimelinePhase::Initializing;
    if (targetTick >= mCurrentTick && !followRecordedPlayer && !forceExportSnapshot) {
        if (exportSeek) mSnapMovementDuringSeek = mExportTimelinePhase == ReplayExportTimelinePhase::Initializing;
        getLogger().debug(
            "Fast-forwarding replay from tick {} to tick {} without reloading snapshots",
            mCurrentTick,
            targetTick
        );
        return;
    }

    if (forceExportSnapshot) {
        getLogger().debug(
            "Rebuilding replay snapshot for export initialization at tick {} (snapshotTick={})",
            targetTick,
            selectedStart
        );
    }

    mReaderIndex = selectedReader;
    mCurrentTick = selectedStart;
    applySnapshot(*mReaders[mReaderIndex], followRecordedPlayer);
    getLogger()
        .debug("Seeking replay to tick {} from snapshot {} at tick {}", targetTick, selectedReader, selectedStart);
}

void ReplaySession::tick() {
    if (!mActive) return;
    try {
        if (!mReplayWorldJoined || !mNetworkHandler || !refreshReplayPlayer()) return;
        if (mReplayTime && mReplayPlayer) mReplayPlayer->getLevel().setTime(*mReplayTime);

        if (mPendingSnapshotApply) {
            if (mPendingReplayDimension) return;
            auto pending = *mPendingSnapshotApply;
            mPendingSnapshotApply.reset();
            if (pending.readerIndex >= mReaders.size()) {
                throw std::runtime_error("Pending replay snapshot index is out of range");
            }
            if (pending.dimensionGeneration != 0 && pending.dimensionGeneration != mCompletedDimensionGeneration) {
                throw std::runtime_error("Pending replay snapshot does not match the completed dimension transition");
            }
            applySnapshot(*mReaders[pending.readerIndex], pending.followRecordedPlayer, pending.serverPlayerRelocated);
            return;
        }

        if (mPendingReplayDimension) return;

        if (!mWorldReady) {
            onWorldReady();
            return;
        }
        if (mChunkInjectionPending) {
            if (!tryFinishChunkInjection()) {
                if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
                return;
            }
            if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
        }

        int const requestedSeek = mRequestedSeekTick.exchange(-1, std::memory_order_acq_rel);
        if (requestedSeek >= 0) {
            beginSeek(requestedSeek);
            if (mChunkInjectionPending || mPendingSnapshotApply || mPendingReplayDimension) return;
        }
        if (mChunkInjectionPending || mPendingSnapshotApply || mPendingReplayDimension) return;

        if (mSeekTargetTick >= 0) {
            int advancedTicks = 0;
            while (mCurrentTick < mSeekTargetTick && !mChunkInjectionPending && !mPendingSnapshotApply
                   && !mPendingReplayDimension && advancedTicks < SeekTicksPerClientTick) {
                if (!advanceReplayTick(false)) {
                    getLogger().warn("Replay ended at tick {} while seeking to tick {}", mCurrentTick, mSeekTargetTick);
                    mSeekTargetTick         = -1;
                    mExportSeekRequested    = false;
                    mSnapMovementDuringSeek = false;
                    return;
                }
                ++advancedTicks;
            }
            if (mCurrentTick >= mSeekTargetTick) {
                getLogger().debug("Replay seek completed at tick {}", mCurrentTick);
                mSeekTargetTick         = -1;
                mExportSeekRequested    = false;
                mSnapMovementDuringSeek = false;
            }
            return;
        }

        if (mExportTimelinePhase == ReplayExportTimelinePhase::Continuous) {
            if (mCurrentTick > mExportTargetTick) {
                throw std::runtime_error("Continuous export replay tick moved past its frame target");
            }
            if (mCurrentTick < mExportTargetTick && !advanceReplayTick(false)) {
                throw std::runtime_error("Replay ended before the continuous export frame target");
            }
            return;
        }

        if (mIsPaused) return;
        mPlaybackTickAccumulator += mPlaybackSpeed;
        int const ticksToAdvance  = static_cast<int>(mPlaybackTickAccumulator);
        mPlaybackTickAccumulator -= static_cast<float>(ticksToAdvance);
        for (int tick = 0;
             tick < ticksToAdvance && !mChunkInjectionPending && !mPendingSnapshotApply && !mPendingReplayDimension;
             ++tick) {
            if (!advanceReplayTick(true)) break;
        }
    } catch (std::exception const& e) {
        getLogger().error("Replay session failed: {}", e.what());
        stop();
    }
}

void ReplaySession::updateControlPlane() {
    if (mStopRequested.exchange(false, std::memory_order_acq_rel)) {
        if (mActive) stop();
        return;
    }
    if (!mActive || !mReplayWorldJoined || !mPendingReplayDimension) return;

    try {
        auto request = mDimensionTransitionRequest;
        if (!request) throw std::runtime_error("Replay dimension transition lost its server request");

        if (mRequestedSeekTick.load(std::memory_order_acquire) >= 0) {
            auto expected = DimensionTransitionStatus::Pending;
            if (request->status.compare_exchange_strong(
                    expected,
                    DimensionTransitionStatus::Cancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                mPendingSnapshotApply.reset();
                mPendingReplayDimension.reset();
                mDimensionTransitionRequest.reset();
                mDimensionTransitionStartedAt      = {};
                mDimensionTransitionSettledUpdates = 0;
                mSeekTargetTick                    = -1;
                mExportSeekRequested               = false;
                mSnapMovementDuringSeek            = false;
                if (!mChunkInjectionPending) resetDimensionScopedReplayState();
                (void)refreshReplayPlayer();
                return;
            }
        }

        processPendingDimensionTransition();
    } catch (std::exception const& e) {
        getLogger().error("Replay control plane failed: {}", e.what());
        mReplayFailed = true;
        stop();
    } catch (...) {
        getLogger().error("Replay control plane failed");
        mReplayFailed = true;
        stop();
    }
}

bool ReplaySession::init(std::filesystem::path filePath) {
    auto path       = filePath.string();
    int  errorCode  = 0;
    auto rawArchive = zip_open(path.c_str(), ZIP_RDONLY, &errorCode);
    if (!rawArchive) {
        getLogger().error("Unable to open replay archive: {}", filePath);
        return false;
    }
    std::unique_ptr<zip_t, decltype(&zip_close)> archive(rawArchive, &zip_close);

    auto metadata = readArchiveEntry(archive.get(), "metadata.json");
    if (!metadata) {
        getLogger().error("Replay archive does not contain metadata.json");
        return false;
    }

    mMeta = PlaybackMeta::fromJson(*metadata);
    if (mMeta.chunks.empty()) {
        getLogger().error("Replay archive does not contain replay chunks");
        return false;
    }
    mReaders.clear();
    mSnapshotContexts.clear();
    mChunkPackets.clear();
    mInlineLevelChunkPacketIndices.clear();
    mInlineSubChunkPacketIndices.clear();

    for (auto const& [chunkName, _] : mMeta.chunks) {
        auto chunk = readArchiveEntry(archive.get(), chunkName);
        if (!chunk) {
            getLogger().error("Replay archive does not contain {}", chunkName);
            return false;
        }
        auto reader  = std::make_unique<ReplayReader>(*chunk);
        auto context = reader->readSnapshotContext();
        mReaders.emplace_back(std::move(reader));
        mSnapshotContexts.emplace_back(context);
    }
    for (int cacheIndex = 0;; ++cacheIndex) {
        auto entryName = "level_chunk_caches/" + std::to_string(cacheIndex) + ".bin";
        if (zip_name_locate(archive.get(), entryName.c_str(), 0) < 0) break;

        auto cache = readArchiveEntry(archive.get(), entryName);
        if (!cache || !appendChunkCache(*cache, mChunkPackets)) {
            getLogger().error("Unable to read replay chunk cache {}", entryName);
            return false;
        }
    }

    mReplayFilePath          = std::move(filePath);
    mCurrentTick             = 0;
    mReaderIndex             = 0;
    mSeekTargetTick          = -1;
    mExportSeekRequested     = false;
    mSnapMovementDuringSeek  = false;
    mExportTimelinePhase     = ReplayExportTimelinePhase::Inactive;
    mExportTargetTick        = -1;
    mPlaybackSpeed           = 1.0f;
    mPlaybackTickAccumulator = 0.0f;
    mReplayTime.reset();
    mPendingReplayDimension.reset();
    mPendingSnapshotApply.reset();
    mDimensionTransitionRequest.reset();
    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionGeneration     = 0;
    mCompletedDimensionGeneration      = 0;
    mDimensionTransitionStartedAt      = {};
    mRequestedSeekTick.store(-1, std::memory_order_relaxed);
    mReplayWorldJoined = false;
    mWorldReady        = false;
    mReplayFailed      = false;
    mIsPaused          = true;
    mStopRequested.store(false, std::memory_order_relaxed);
    mInjectingPacket.store(nullptr, std::memory_order_release);
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mReplayDimension.store(nullptr, std::memory_order_release);
    mInitialSnapshotApplied  = false;
    mChunkInjectionPending   = false;
    mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
    mReplayPlayer            = nullptr;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mPendingSnapshotLocalPlayer.reset();
    mPendingSnapshotGamePackets.clear();
    mAppliedConfigurationPackets.clear();
    mRecordedEntityIds.clear();
    mEntityRenderKeys.clear();
    render::clearReplayEntityPoses();
    mReplayObjectiveNames.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mApplyingChunkSnapshot      = false;
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionStartedAt    = {};
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
    clearNetworkContext();
    return true;
}

bool ReplaySession::prepareReplayResourcePacks(std::vector<PlaybackSerializedGamePacket> const& packets) {
    mReplayResourcePacksInfo.store(nullptr, std::memory_order_release);
    mReplayResourcePackStack.store(nullptr, std::memory_order_release);
    mReplayCachedResourcePacksLoaded = false;

    auto client = ll::service::getClientInstance();
    if (!client) return false;

    auto findPacket = [&packets](MinecraftPacketIds packetId) -> PlaybackSerializedGamePacket const* {
        for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
            if (it->mPacketId == static_cast<int32_t>(packetId)) return &*it;
        }
        return nullptr;
    };
    auto decodePacket = [](PlaybackSerializedGamePacket const& serialized) {
        auto packet = MinecraftPackets::createPacket(static_cast<MinecraftPacketIds>(serialized.mPacketId));
        if (!packet) throw std::runtime_error("Unable to create a recorded resource-pack packet");

        ReadOnlyBinaryStream stream(serialized.mPayload, false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) {
            throw std::runtime_error(
                std::format("Unable to decode recorded resource-pack packet {}", serialized.mPacketId)
            );
        }
        return packet;
    };

    auto&       repository      = client->getResourcePackRepository();
    auto const* serializedInfo  = findPacket(MinecraftPacketIds::ResourcePacksInfo);
    auto const* serializedStack = findPacket(MinecraftPacketIds::ResourcePackStack);

    std::shared_ptr<ResourcePacksInfoPacket> info;
    if (serializedInfo) {
        info      = std::static_pointer_cast<ResourcePacksInfoPacket>(decodePacket(*serializedInfo));
        auto keys = info->mData->collectKeys();
        mReplayCachedResourcePacksLoaded = true;
        repository.addCachedResourcePacks(&keys);
    }

    if (!serializedStack) return true;

    auto stack = std::static_pointer_cast<ResourcePackStackPacket>(decodePacket(*serializedStack));
    if (!info) return true;
    mReplayResourcePacksInfo.store(std::move(info), std::memory_order_release);
    mReplayResourcePackStack.store(std::move(stack), std::memory_order_release);
    return true;
}

void ReplaySession::releaseReplayResourcePacks() {
    mReplayResourcePacksInfo.store(nullptr, std::memory_order_release);
    mReplayResourcePackStack.store(nullptr, std::memory_order_release);
    if (!mReplayCachedResourcePacksLoaded) return;
    mReplayCachedResourcePacksLoaded = false;

    auto client = ll::service::getClientInstance();
    if (!client) return;
    try {
        client->getResourcePackRepository().removePacksLoadedFromCache();
    } catch (std::exception const& exception) {
        getLogger().error("Unable to release replay resource packs loaded from cache: {}", exception.what());
    } catch (...) {
        getLogger().error("Unable to release replay resource packs loaded from cache");
    }
}

void ReplaySession::onWorldReady() {
    if (!mInitialSnapshotApplied) {
        applyInitialSnapshot();
        mInitialSnapshotApplied = true;
        if (mReplayFailed) throw std::runtime_error("Unable to apply replay snapshot");
        return;
    }

    if (mChunkInjectionPending && !tryFinishChunkInjection()) {
        if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");
        return;
    }
    if (mReplayFailed) throw std::runtime_error("Unable to apply replay chunks");

    mWorldReady = true;
    getLogger().info("Replay ready at tick {} ({})", mCurrentTick, mIsPaused ? "paused" : "playing");
}

void ReplaySession::applyInitialSnapshot() {
    if (mReaders.empty()) throw std::runtime_error("Replay contains no chunks");

    applySnapshot(*mReaders.front(), true, true);
}

void ReplaySession::applySnapshot(ReplayReader& reader, bool followRecordedPlayer, bool serverPlayerRelocated) {
    if (mChunkInjectionPending) throw std::runtime_error("Previous replay snapshot is still being applied");

    if (!refreshReplayPlayer()) throw std::runtime_error("Replay player is unavailable while applying snapshot");
    auto* replayPlayer = mReplayPlayer;

    if (mReaderIndex >= mSnapshotContexts.size()) {
        throw std::runtime_error("Replay snapshot context index is out of range");
    }
    auto const&        snapshotContext = mSnapshotContexts[mReaderIndex];
    PlaybackView const snapshotView{
        snapshotContext.x,
        snapshotContext.y,
        snapshotContext.z,
        snapshotContext.yaw,
        snapshotContext.pitch
    };
    PlaybackView transitionView = snapshotView;
    if (!followRecordedPlayer) {
        auto const& position = replayPlayer->getPosition();
        auto const& rotation = replayPlayer->getRotation();
        transitionView       = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    }
    bool const relocateWithinDimension = followRecordedPlayer && !serverPlayerRelocated;
    if (!ensureReplayDimension(DimensionType{snapshotContext.dimensionId}, transitionView, relocateWithinDimension)) {
        if (!mReplayFailed) {
            auto const generation = mDimensionTransitionRequest ? mDimensionTransitionRequest->generation : 0;
            mPendingSnapshotApply = PendingSnapshotApply{
                mReaderIndex,
                followRecordedPlayer,
                serverPlayerRelocated || followRecordedPlayer,
                generation
            };
        }
        return;
    }

    mPendingSnapshotLocalPlayer.reset();
    mPendingSnapshotGamePackets.clear();
    if (!clearRecordedEntities()) {
        mReplayFailed = true;
        return;
    }
    if (!clearReplayObjectives()) {
        mReplayFailed = true;
        return;
    }

    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
    }
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
    mChunkInjectionPending   = false;
    mApplyingChunkSnapshot   = true;
    mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
    mApplyingSnapshotChunks.clear();
    mPendingSnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mInjectedLevelChunks     = 0;
    mInjectedSubChunkPackets = 0;
    mInjectedSubChunkEntries = 0;
    mReusedSnapshotColumns   = 0;
    mDirectLevelChunks       = 0;
    mDirectSubChunkPackets   = 0;
    mDirectSubChunkEntries   = 0;

    reader.handleSnapshot(*this);
    reader.resetToStart();
    if (mReplayFailed) {
        mApplyingChunkSnapshot = false;
        return;
    }
    if (!mPendingSnapshotLocalPlayer) {
        getLogger().error("Replay snapshot {} is missing its CreateLocalPlayer action", mReaderIndex);
        mReplayFailed          = true;
        mApplyingChunkSnapshot = false;
        return;
    }

    PlaybackView view;
    if (followRecordedPlayer) {
        view = snapshotView;
        replayPlayer->moveTo(Vec3{view.x, view.y, view.z}, Vec2{view.pitch, view.yaw});
    } else if (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive && mExportCameraViewpoint) {
        auto const& position = *mExportCameraViewpoint;
        auto const& rotation = replayPlayer->getRotation();
        view                 = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    } else {
        auto const& position = replayPlayer->getPosition();
        auto const& rotation = replayPlayer->getRotation();
        view                 = PlaybackView{position.x, position.y, position.z, rotation.y, rotation.x};
    }
    mChunkInjectionStartedAt = std::chrono::steady_clock::now();
    if (!prepareChunkInjectionPlan(view)) {
        mReplayFailed          = true;
        mApplyingChunkSnapshot = false;
        return;
    }
    mChunkPlanPreparationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt).count();
    getLogger().debug(
        "Prepared replay snapshot {} around ({:.3f}, {:.3f}, {:.3f}) before chunk injection (centerSource={})",
        mReaderIndex,
        view.x,
        view.y,
        view.z,
        followRecordedPlayer ? "recorded-player"
                             : (mExportTimelinePhase != ReplayExportTimelinePhase::Inactive && mExportCameraViewpoint
                                    ? "export-camera"
                                    : "operator-player")
    );

    mChunkInjectionPending = true;
    getLogger().debug(
        "Starting distance-prioritized replay chunk streaming with {} columns, {} SubChunk packets, and at most {} "
        "LevelChunks in flight (plan {:.3f} ms)",
        mPendingLevelChunkIndices.size(),
        mPendingSubChunkPackets.size(),
        MAX_LEVEL_CHUNKS_IN_FLIGHT,
        mChunkPlanPreparationMs
    );
}

bool ReplaySession::ensureReplayDimension(
    DimensionType       target,
    PlaybackView const& view,
    bool                relocateWithinDimension
) {
    if (!mReplayPlayer) return false;
    bool const changesDimension = mReplayPlayer->getDimensionId() != target;
    if (!changesDimension && !relocateWithinDimension) {
        mReplayDimension.store(&mReplayPlayer->getDimension(), std::memory_order_release);
        return true;
    }
    if (mPendingReplayDimension) {
        if (*mPendingReplayDimension != target) {
            getLogger().error(
                "Replay requested dimension {} while dimension {} is still pending",
                target.id,
                mPendingReplayDimension->id
            );
            mReplayFailed = true;
        }
        return false;
    }

    auto       request         = std::make_shared<DimensionTransitionRequest>();
    auto       playerUuid      = mReplayPlayer->getUuid();
    auto       replayLevelId   = mReplayLevelId;
    auto       sourceDimension = mReplayPlayer->getDimensionId();
    auto       position        = Vec3{view.x, view.y, view.z};
    auto       rotation        = Vec2{view.pitch, view.yaw};
    auto const generation      = mDimensionTransitionGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    request->generation        = generation;
    auto* generationCounter    = &mDimensionTransitionGeneration;

    if (!clearRecordedEntities()) {
        getLogger().error("Unable to clear recorded entities before leaving replay dimension {}", sourceDimension.id);
        mReplayFailed = true;
        return false;
    }

    // Stop applying source-dimension protection before destination packets can arrive.
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mReplayDimension.store(nullptr, std::memory_order_release);

    mPendingReplayDimension            = target;
    mDimensionTransitionRequest        = request;
    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionStartedAt      = std::chrono::steady_clock::now();
    getLogger().info(
        "Replay dimension transition generation {} started at tick {} from dimension {} to {}",
        generation,
        mCurrentTick,
        sourceDimension.id,
        target.id
    );
    ll::thread::ServerThreadExecutor::getDefault().execute([request,
                                                            generation,
                                                            generationCounter,
                                                            playerUuid,
                                                            replayLevelId = std::move(replayLevelId),
                                                            position,
                                                            rotation,
                                                            target] {
        if (generationCounter->load(std::memory_order_acquire) != generation) return;
        auto expected = DimensionTransitionStatus::Pending;
        if (!request->status.compare_exchange_strong(
                expected,
                DimensionTransitionStatus::Dispatching,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return;
        }

        auto fail = [&request] {
            auto expectedStatus = DimensionTransitionStatus::Dispatching;
            request->status.compare_exchange_strong(
                expectedStatus,
                DimensionTransitionStatus::Failed,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            );
        };

        try {
            if (generationCounter->load(std::memory_order_acquire) != generation) return;
            auto level = ll::service::getLevel();
            if (!level || level->getLevelId() != replayLevelId) {
                getLogger().error("Unable to change replay dimension: replay server level is unavailable");
                fail();
                return;
            }

            auto  targetDimension = level->getOrCreateDimension(target).lock();
            auto* player          = level->getPlayer(playerUuid);
            if (!targetDimension || !player) {
                getLogger().error(
                    "Unable to change replay dimension to {}: target dimension or server player is unavailable",
                    target.id
                );
                fail();
                return;
            }

            if (generationCounter->load(std::memory_order_acquire) != generation) {
                fail();
                return;
            }

            // Revisited dimensions need a fresh native LevelChunk publication.
            auto const& chunkPublisher = player->mChunkPublisherView.get();
            if (chunkPublisher) chunkPublisher->clearRegion();
            player->teleport(position, target, rotation);
            if (generationCounter->load(std::memory_order_acquire) != generation) {
                fail();
                return;
            }
            auto expectedStatus = DimensionTransitionStatus::Dispatching;
            if (!request->status.compare_exchange_strong(
                    expectedStatus,
                    DimensionTransitionStatus::Succeeded,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                )) {
                return;
            }
        } catch (std::exception const& e) {
            getLogger().error("Unable to change replay dimension to {}: {}", target.id, e.what());
            fail();
        } catch (...) {
            getLogger().error("Unable to change replay dimension to {}", target.id);
            fail();
        }
    });
    return false;
}

void ReplaySession::processPendingDimensionTransition() {
    if (!mPendingReplayDimension) return;
    if (!mDimensionTransitionRequest) {
        mReplayFailed = true;
        throw std::runtime_error("Replay dimension transition lost its server request");
    }

    auto const status = mDimensionTransitionRequest->status.load(std::memory_order_acquire);
    if (status == DimensionTransitionStatus::Failed || status == DimensionTransitionStatus::Cancelled) {
        mReplayFailed = true;
        throw std::runtime_error("Replay server dimension transition failed");
    }

    auto const elapsed          = std::chrono::steady_clock::now() - mDimensionTransitionStartedAt;
    bool const playerAvailable  = refreshReplayPlayer();
    bool const dimensionMatches = playerAvailable && mReplayPlayer->getDimensionId() == *mPendingReplayDimension;
    auto const waitComponent =
        playerAvailable ? mReplayPlayer->getEntityContext().tryGetComponent<LocalPlayerDimensionWaitComponent>()
                        : nullptr;
    bool const waitingForAcknowledgment =
        waitComponent && waitComponent->mWaitingForServerDimensionChangeAcknowledgment;
    auto const client               = ll::service::getClientInstance();
    bool const loadingScreenVisible = client
                                   && (client->isShowingLoadingScreen() || client->isShowingProgressScreen()
                                       || client->isShowingWorldProgressScreen());
    bool const readyToRender        = client && client->isReadyToRender();

    if (elapsed >= DIMENSION_TRANSITION_TIMEOUT) {
        getLogger().error(
            "Replay dimension transition generation {} to {} timed out after {} ms with request status {}; "
            "playerAvailable={}, currentDimension={}, dimensionMatches={}, waitingForAck={}, loadingScreen={}, "
            "readyToRender={}",
            mDimensionTransitionRequest->generation,
            mPendingReplayDimension->id,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            static_cast<int>(status),
            playerAvailable,
            playerAvailable ? mReplayPlayer->getDimensionId().id : 0,
            dimensionMatches,
            waitingForAcknowledgment,
            loadingScreenVisible,
            readyToRender
        );
        mReplayFailed = true;
        throw std::runtime_error("Replay dimension transition timed out");
    }

    if (!playerAvailable) {
        mDimensionTransitionSettledUpdates = 0;
        return;
    }

    if (status == DimensionTransitionStatus::Succeeded && dimensionMatches && waitingForAcknowledgment
        && elapsed >= DIMENSION_ACK_FALLBACK_DELAY) {
        if (client) {
            bool expected = false;
            if (mDimensionTransitionRequest->acknowledgmentFallbackQueued
                    .compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
                PlayerActionPacket acknowledgment{};
                acknowledgment.mAction    = PlayerActionType::ChangeDimensionAck;
                acknowledgment.mRuntimeId = mReplayPlayer->getRuntimeID();
                client->getPacketSender().sendToServer(acknowledgment);
                getLogger().debug(
                    "Sent replay dimension acknowledgment fallback from the client for generation {}",
                    mDimensionTransitionRequest->generation
                );
            }
        }
    }

    bool const acknowledgmentGraceComplete =
        !waitingForAcknowledgment
        || mDimensionTransitionRequest->acknowledgmentFallbackQueued.load(std::memory_order_acquire);
    if (status != DimensionTransitionStatus::Succeeded || !dimensionMatches || !acknowledgmentGraceComplete) {
        mDimensionTransitionSettledUpdates = 0;
        return;
    }

    if (waitingForAcknowledgment && mDimensionTransitionSettledUpdates == 0) {
        getLogger().warn(
            "Replay dimension transition generation {} reached dimension {} but its client acknowledgment remains "
            "pending; continuing after the fallback grace period",
            mDimensionTransitionRequest->generation,
            mPendingReplayDimension->id
        );
    }

    if (!client) {
        mDimensionTransitionSettledUpdates = 0;
        return;
    }

    if (loadingScreenVisible && mDimensionTransitionSettledUpdates == 0) {
        getLogger().info(
            "Replay dimension transition generation {} reached dimension {} while the loading screen remains "
            "visible; resuming destination snapshot injection (readyToRender={})",
            mDimensionTransitionRequest->generation,
            mPendingReplayDimension->id,
            readyToRender
        );
    }

    if (++mDimensionTransitionSettledUpdates >= DIMENSION_TRANSITION_SETTLE_UPDATES) {
        completeReplayDimensionTransition();
    }
}

void ReplaySession::completeReplayDimensionTransition() {
    if (!refreshReplayPlayer()) return;
    auto const completedGeneration = mDimensionTransitionRequest->generation;
    auto const elapsed             = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - mDimensionTransitionStartedAt
    );
    getLogger().info(
        "Replay dimension transition generation {} completed at tick {} in dimension {} after {} ms",
        completedGeneration,
        mCurrentTick,
        mReplayPlayer->getDimensionId().id,
        elapsed.count()
    );
    mReplayDimension.store(&mReplayPlayer->getDimension(), std::memory_order_release);
    mPendingReplayDimension.reset();
    mDimensionTransitionRequest.reset();
    mDimensionTransitionSettledUpdates = 0;
    mDimensionTransitionStartedAt      = {};
    mCompletedDimensionGeneration      = completedGeneration;
    resetDimensionScopedReplayState();
}

void ReplaySession::resetDimensionScopedReplayState() {
    mChunkInjectionPending      = false;
    mChunkInjectionPlanPrepared = false;
    mApplyingChunkSnapshot      = false;
    mCenterChunksReady          = false;
    mSnapshotGamePacketPhase    = SnapshotGamePacketPhase::StreamingChunks;
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    mPendingSnapshotLocalPlayer.reset();
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mChunkIsolationDimension.reset();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mCenterChunkPositions.clear();
    mRecordedEntityIds.clear();
    mEntityRenderKeys.clear();
    render::clearReplayEntityPoses();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
}

bool ReplaySession::refreshReplayPlayer() {
    auto  client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !isReplayLevel(player->getLevel())) return false;

    mReplayPlayer = player;
    if (mPendingReplayDimension && player->getDimensionId() != *mPendingReplayDimension) {
        mReplayDimension.store(nullptr, std::memory_order_release);
    } else {
        mReplayDimension.store(&player->getDimension(), std::memory_order_release);
    }
    return true;
}

bool ReplaySession::prepareChunkInjectionPlan(PlaybackView const& view) {
    struct PrioritizedLevelChunk {
        ChunkPos pos;
        int64_t  distanceSquared;
        int      index;
    };
    struct PrioritizedSubChunk {
        PendingSubChunkPacket packet;
        int64_t               distanceSquared;
    };

    int const  centerX         = static_cast<int>(std::floor(view.x / 16.0f));
    int const  centerZ         = static_cast<int>(std::floor(view.z / 16.0f));
    auto const distanceSquared = [centerX, centerZ](ChunkPos const& pos) {
        int64_t const dx = static_cast<int64_t>(pos.x) - centerX;
        int64_t const dz = static_cast<int64_t>(pos.z) - centerZ;
        return dx * dx + dz * dz;
    };

    std::vector<PrioritizedLevelChunk>                    levelChunks;
    std::unordered_set<ChunkPos>                          levelChunkPositions;
    std::unordered_set<ChunkPos>                          requestModeLevelChunks;
    std::unordered_map<ChunkPos, SnapshotColumnIdentity>  targetColumns;
    std::unordered_map<ChunkPos, std::unordered_set<int>> subChunkIndicesByColumn;
    size_t                                                skippedBlobCachePackets = 0;
    levelChunks.reserve(mPendingLevelChunkIndices.size());
    levelChunkPositions.reserve(mPendingLevelChunkIndices.size());
    requestModeLevelChunks.reserve(mPendingLevelChunkIndices.size());

    for (int index : mPendingLevelChunkIndices) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
        if (!packet) {
            getLogger().error("Unable to create a LevelChunk packet while preparing replay streaming");
            return false;
        }

        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) {
            getLogger().error("Unable to decode replay LevelChunk packet {} while preparing streaming", index);
            return false;
        }

        auto const& levelChunk = static_cast<LevelChunkPacket const&>(*packet);
        if (static_cast<bool>(levelChunk.mCacheEnabled)) {
            ++skippedBlobCachePackets;
            continue;
        }

        ChunkPos const pos = *levelChunk.mPos;
        if (!levelChunkPositions.emplace(pos).second) {
            getLogger().error("Replay snapshot contains duplicate LevelChunk column ({}, {})", pos.x, pos.z);
            return false;
        }
        if (static_cast<bool>(levelChunk.mClientNeedsToRequestSubchunks)) {
            requestModeLevelChunks.emplace(pos);
        }
        targetColumns[pos].levelChunkIndex = index;
        levelChunks.push_back(PrioritizedLevelChunk{pos, distanceSquared(pos), index});
    }

    std::stable_sort(levelChunks.begin(), levelChunks.end(), [](auto const& left, auto const& right) {
        if (left.distanceSquared != right.distanceSquared) return left.distanceSquared < right.distanceSquared;
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        return left.pos.z < right.pos.z;
    });

    mCenterChunkPositions.clear();
    for (auto const& levelChunk : levelChunks) {
        if (std::abs(levelChunk.pos.x - centerX) <= 2 && std::abs(levelChunk.pos.z - centerZ) <= 2) {
            mCenterChunkPositions.emplace(levelChunk.pos);
        }
    }
    if (mCenterChunkPositions.empty() && !levelChunks.empty()) {
        mCenterChunkPositions.emplace(levelChunks.front().pos);
    }

    auto& protectedChunks = mApplyingChunkSnapshot ? mApplyingSnapshotChunks : mSnapshotChunks;
    protectedChunks.insert(levelChunkPositions.begin(), levelChunkPositions.end());

    std::vector<PrioritizedSubChunk> subChunks;
    subChunks.reserve(mPendingSubChunkIndices.size());
    mRemainingSubChunkPacketsByColumn.clear();
    for (int index : mPendingSubChunkIndices) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
        if (!packet) {
            getLogger().error("Unable to create a SubChunk packet while preparing replay streaming");
            return false;
        }

        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) {
            getLogger().error("Unable to decode replay SubChunk packet {} while preparing streaming", index);
            return false;
        }

        auto const& subChunk = static_cast<SubChunkPacket const&>(*packet);
        if (static_cast<bool>(subChunk.mCacheEnabled)) {
            ++skippedBlobCachePackets;
            continue;
        }

        auto const& entries = *subChunk.mSubChunkData;
        if (entries.empty()) {
            getLogger().error("Replay SubChunk packet {} contains no successful entries", index);
            return false;
        }
        if (entries.size() > MAX_SUB_CHUNK_ENTRIES_PER_PACKET) {
            getLogger().error(
                "Replay SubChunk packet {} has {} entries, exceeding the per-packet limit {}",
                index,
                entries.size(),
                MAX_SUB_CHUNK_ENTRIES_PER_PACKET
            );
            return false;
        }

        PendingSubChunkPacket pending;
        pending.index                                          = index;
        auto const&                                     center = *subChunk.mCenterPos;
        std::vector<SubChunkPacket::SubChunkPacketData> playableEntries;
        playableEntries.reserve(entries.size());
        for (auto const& entry : entries) {
            auto const result = static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult);
            if (!isSuccessfulSubChunkResult(result)) continue;
            auto const&    offset = *entry.mSubChunkPosOffset;
            ChunkPos const target{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)};
            if (!levelChunkPositions.contains(target) && !mSnapshotChunks.contains(target)) {
                if (mApplyingChunkSnapshot) {
                    getLogger().error(
                        "Replay snapshot SubChunk packet {} targets column ({}, {}) without a LevelChunk",
                        index,
                        target.x,
                        target.z
                    );
                    return false;
                }
                continue;
            }

            playableEntries.emplace_back(entry);
            subChunkIndicesByColumn[target].emplace(center.y + static_cast<int>(offset.mY));
            if (std::find(pending.targets.begin(), pending.targets.end(), target) == pending.targets.end()) {
                pending.targets.emplace_back(target);
            }
        }
        if (pending.targets.empty()) continue;
        if (playableEntries.size() != entries.size()) {
            auto filteredPacket           = subChunk;
            *filteredPacket.mSubChunkData = std::move(playableEntries);
            PlaybackBuffer filteredPayload;
            filteredPacket.write(filteredPayload);
            pending.payload = std::move(filteredPayload.mBuffer);
        }
        int64_t priority = std::numeric_limits<int64_t>::max();
        for (auto const& target : pending.targets) {
            targetColumns[target].subChunkIndices.emplace_back(index);
            priority = std::min(priority, distanceSquared(target));
        }
        subChunks.push_back(PrioritizedSubChunk{std::move(pending), priority});
    }

    for (auto& [_, identity] : targetColumns) {
        std::sort(identity.subChunkIndices.begin(), identity.subChunkIndices.end());
        identity.subChunkIndices.erase(
            std::unique(identity.subChunkIndices.begin(), identity.subChunkIndices.end()),
            identity.subChunkIndices.end()
        );
    }

    mReusableSnapshotColumns.clear();
    for (auto const& [pos, identity] : targetColumns) {
        auto applied = mAppliedSnapshotColumns.find(pos);
        if (identity.levelChunkIndex >= 0 && applied != mAppliedSnapshotColumns.end() && applied->second == identity
            && !mDirtySnapshotColumns.contains(pos)) {
            mReusableSnapshotColumns.emplace(pos);
        }
    }
    mReusedSnapshotColumns = mReusableSnapshotColumns.size();

    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) {
        getLogger().error("Replay dimension disappeared while preparing direct chunk updates");
        return false;
    }
    mChunkIsolationDimension = replayDimension->getDimensionId();

    int const    minimumSubChunk = static_cast<int>(replayDimension->mHeightRange->mMin) / 16;
    size_t const subChunkCount   = static_cast<size_t>(replayDimension->getHeightInSubchunks());
    if (subChunkCount == 0) {
        getLogger().error("Replay dimension has no subchunk slots");
        return false;
    }

    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    auto& chunkSource = replayDimension->getChunkSource();
    for (auto const& [pos, identity] : targetColumns) {
        if (mReusableSnapshotColumns.contains(pos) || identity.levelChunkIndex < 0
            || !requestModeLevelChunks.contains(pos)) {
            continue;
        }

        auto covered = subChunkIndicesByColumn.find(pos);
        if (covered == subChunkIndicesByColumn.end() || covered->second.size() != subChunkCount) continue;
        bool const coversCompleteHeight =
            std::all_of(covered->second.begin(), covered->second.end(), [minimumSubChunk, subChunkCount](int index) {
                return index >= minimumSubChunk && static_cast<size_t>(index - minimumSubChunk) < subChunkCount;
            });
        if (!coversCompleteHeight) continue;

        auto chunk = chunkSource.getExistingChunk(pos);
        if (!chunk || chunk->mIsEmptyClientChunk
            || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded
            || chunk->mSubChunks->size() != subChunkCount) {
            continue;
        }

        mDirectSnapshotColumns.emplace(pos);
        mDirectLevelChunkIndices.emplace(identity.levelChunkIndex);
    }

    std::unordered_set<ChunkPos> queuedLevelChunkPositions;
    mPendingLevelChunkIndices.clear();
    mPendingLevelChunkIndices.reserve(levelChunks.size() - mReusedSnapshotColumns);
    for (auto const& levelChunk : levelChunks) {
        if (mReusableSnapshotColumns.contains(levelChunk.pos)) continue;
        mPendingLevelChunkIndices.emplace_back(levelChunk.index);
        queuedLevelChunkPositions.emplace(levelChunk.pos);
    }

    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mCompletedLevelChunkPositions.insert(mReusableSnapshotColumns.begin(), mReusableSnapshotColumns.end());
    }

    std::stable_sort(subChunks.begin(), subChunks.end(), [](auto const& left, auto const& right) {
        return left.distanceSquared < right.distanceSquared;
    });
    mPendingSubChunkPackets.clear();
    mPendingSubChunkPackets.reserve(subChunks.size());
    for (auto& subChunk : subChunks) {
        bool const reusable =
            std::all_of(subChunk.packet.targets.begin(), subChunk.packet.targets.end(), [this](ChunkPos const& pos) {
                return mReusableSnapshotColumns.contains(pos);
            });
        if (reusable) continue;

        for (auto const& target : subChunk.packet.targets) {
            if (queuedLevelChunkPositions.contains(target)) subChunk.packet.dependencies.emplace_back(target);
            ++mRemainingSubChunkPacketsByColumn[target];
        }
        mPendingSubChunkPackets.emplace_back(std::move(subChunk.packet));
    }
    mPendingSubChunkIndices.clear();

    if (skippedBlobCachePackets != 0) {
        getLogger().warn(
            "Skipped {} replay chunk packets that depend on unavailable server blob-cache data",
            skippedBlobCachePackets
        );
    }

    mPendingSnapshotColumns = std::move(targetColumns);
    if (mApplyingChunkSnapshot || !levelChunks.empty()) {
        getLogger().debug(
            "Prepared replay {} chunk batch with {} target columns: {} reused, {} direct, {} native",
            mApplyingChunkSnapshot ? "snapshot" : "timeline",
            mPendingSnapshotColumns.size(),
            mReusedSnapshotColumns,
            mDirectSnapshotColumns.size(),
            levelChunks.size() - mReusedSnapshotColumns - mDirectSnapshotColumns.size()
        );
    }

    mPendingLevelChunkCursor    = 0;
    mPendingSubChunkCursor      = 0;
    mChunkInjectionPlanPrepared = true;
    return true;
}

bool ReplaySession::tryFinishChunkInjection() {
    if (!mChunkInjectionPending) return true;
    if (mSnapshotGamePacketPhase == SnapshotGamePacketPhase::WaitingAfterPlayerList) {
        auto const deadline = std::chrono::steady_clock::now() + SnapshotGamePacketBudget;
        if (!flushPendingSnapshotGamePackets(false, SNAPSHOT_GAME_PACKETS_PER_TICK, deadline)) {
            mReplayFailed = true;
            return false;
        }
        if (!mPendingSnapshotGamePackets.empty()) return false;
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::WaitingAfterEntities;
        return false;
    }
    if (mSnapshotGamePacketPhase == SnapshotGamePacketPhase::WaitingAfterEntities) {
        mAppliedSnapshotColumns = std::move(mPendingSnapshotColumns);
        mDirtySnapshotColumns.clear();
        mReusableSnapshotColumns.clear();
        mDirectSnapshotColumns.clear();
        mDirectLevelChunkIndices.clear();
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
        mChunkInjectionPending   = false;
        return true;
    }
    if (!mChunkInjectionPlanPrepared) {
        auto* player = mReplayPlayer;
        if (!player) {
            mReplayFailed = true;
            return false;
        }
        auto const&  rotation       = player->getRotation();
        auto const&  playerPosition = player->getPosition();
        auto const   position = mExportTimelinePhase != ReplayExportTimelinePhase::Inactive && mExportCameraViewpoint
                                  ? *mExportCameraViewpoint
                                  : ReplayCameraViewpoint{playerPosition.x, playerPosition.y, playerPosition.z};
        PlaybackView view{position.x, position.y, position.z, rotation.y, rotation.x};
        mChunkInjectionStartedAt = std::chrono::steady_clock::now();
        if (!prepareChunkInjectionPlan(view)) {
            mReplayFailed = true;
            return false;
        }
        mChunkPlanPreparationMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt)
                .count();
    }

    bool const   completionProgress = mChunkCompletionObserved.exchange(false, std::memory_order_acq_rel);
    size_t const levelCursorBefore  = mPendingLevelChunkCursor;
    size_t const subCursorBefore    = mPendingSubChunkCursor;

    ++mChunkInjectionTicks;
    auto const injectionStarted = std::chrono::steady_clock::now();
    auto const deadline =
        injectionStarted + (mCenterChunksReady ? OuterChunkInjectionBudget : CenterChunkInjectionBudget);
    size_t injectedSubChunkPackets = 0;
    if (!injectReadySubChunkPackets(injectedSubChunkPackets, deadline) || !injectPendingLevelChunks(deadline)
        || !injectReadySubChunkPackets(injectedSubChunkPackets, deadline)) {
        return false;
    }
    updateCenterChunkReadiness();
    mChunkInjectionDurationsMs.emplace_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - injectionStarted).count()
    );

    size_t completedAfter;
    size_t inFlight;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completedAfter = mCompletedLevelChunkPositions.size();
        inFlight       = mPendingLevelChunks.size();
    }

    bool const allLevelsInjected    = mPendingLevelChunkCursor >= mPendingLevelChunkIndices.size();
    bool const allSubChunksInjected = mPendingSubChunkCursor >= mPendingSubChunkPackets.size();
    if (allLevelsInjected && inFlight == 0 && allSubChunksInjected) return finishChunkInjection();

    bool const madeProgress = completionProgress || mPendingLevelChunkCursor != levelCursorBefore
                           || mPendingSubChunkCursor != subCursorBefore;
    if (madeProgress) mChunkInjectionIdleTicks = 0;
    else ++mChunkInjectionIdleTicks;

    if (mChunkInjectionTicks == 1 || mChunkInjectionTicks % 20 == 0) {
        getLogger().debug(
            "Streaming replay chunks: LevelChunk {}/{} queued, {} completed, {} in flight; SubChunk {}/{} injected",
            mPendingLevelChunkCursor,
            mPendingLevelChunkIndices.size(),
            completedAfter,
            inFlight,
            mPendingSubChunkCursor,
            mPendingSubChunkPackets.size()
        );
    }
    if (mChunkInjectionIdleTicks >= CHUNK_INJECTION_STALL_TIMEOUT_TICKS) {
        getLogger().error(
            "Replay chunk streaming made no progress for {} ticks ({} LevelChunks in flight, SubChunk {}/{})",
            mChunkInjectionIdleTicks,
            inFlight,
            mPendingSubChunkCursor,
            mPendingSubChunkPackets.size()
        );
        mReplayFailed = true;
    }
    return false;
}

bool ReplaySession::injectPendingLevelChunks(std::chrono::steady_clock::time_point deadline) {
    size_t processed = 0;
    while (mPendingLevelChunkCursor < mPendingLevelChunkIndices.size()) {
        if (processed != 0 && std::chrono::steady_clock::now() >= deadline) break;

        int const  index  = mPendingLevelChunkIndices[mPendingLevelChunkCursor];
        bool const direct = mDirectLevelChunkIndices.contains(index);
        if (!direct) {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            if (mPendingLevelChunks.size() >= MAX_LEVEL_CHUNKS_IN_FLIGHT) break;
        }

        bool applied = false;
        if (direct) {
            applied = applyRequestModeLevelChunkDirect(mChunkPackets[static_cast<size_t>(index)]);
            if (!applied) {
                getLogger().warn("Direct replay LevelChunk update became unavailable; falling back to native loading");
                mDirectLevelChunkIndices.clear();
                mDirectSnapshotColumns.clear();
                continue;
            }
        } else {
            applied = injectChunkPacket(mChunkPackets[static_cast<size_t>(index)], MinecraftPacketIds::FullChunkData);
        }
        if (!applied) {
            getLogger().error(
                "Unable to {} replay LevelChunk packet {} of {}",
                direct ? "apply directly" : "inject",
                mPendingLevelChunkCursor,
                mPendingLevelChunkIndices.size()
            );
            mReplayFailed = true;
            return false;
        }
        ++mPendingLevelChunkCursor;
        ++processed;
    }
    return true;
}

bool ReplaySession::injectReadySubChunkPackets(
    size_t&                               injectedPackets,
    std::chrono::steady_clock::time_point deadline
) {
    std::unordered_set<ChunkPos> completed;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completed = mCompletedLevelChunkPositions;
    }

    for (auto& pending : mPendingSubChunkPackets) {
        if (pending.injected) continue;
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (!std::all_of(pending.dependencies.begin(), pending.dependencies.end(), [&completed](ChunkPos const& pos) {
                return completed.contains(pos);
            })) {
            continue;
        }
        bool const direct = std::all_of(pending.targets.begin(), pending.targets.end(), [this](ChunkPos const& pos) {
            return mDirectSnapshotColumns.contains(pos) || mReusableSnapshotColumns.contains(pos);
        });
        std::string_view payload = pending.payload.empty()
                                     ? std::string_view{mChunkPackets[static_cast<size_t>(pending.index)]}
                                     : std::string_view{pending.payload};
        bool             applied =
            direct ? applySubChunkDirect(payload) : injectChunkPacket(payload, MinecraftPacketIds::SubChunkPacket);
        if (direct && !applied) {
            getLogger().warn("Direct replay SubChunk update became unavailable; falling back to native loading");
            for (auto const& target : pending.targets) mDirectSnapshotColumns.erase(target);
            applied = injectChunkPacket(payload, MinecraftPacketIds::SubChunkPacket);
        }
        if (!applied) {
            getLogger()
                .error("Unable to {} replay SubChunk packet {}", direct ? "apply directly" : "inject", pending.index);
            mReplayFailed = true;
            return false;
        }

        pending.injected = true;
        ++mPendingSubChunkCursor;
        ++injectedPackets;
        for (auto const& target : pending.targets) {
            auto remaining = mRemainingSubChunkPacketsByColumn.find(target);
            if (remaining != mRemainingSubChunkPacketsByColumn.end() && remaining->second != 0) {
                --remaining->second;
            }
        }
    }
    return true;
}

void ReplaySession::updateCenterChunkReadiness() {
    if (mCenterChunksReady || mCenterChunkPositions.empty()) return;

    std::unordered_set<ChunkPos> completed;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completed = mCompletedLevelChunkPositions;
    }
    for (auto const& pos : mCenterChunkPositions) {
        if (!completed.contains(pos)) return;
        auto remaining = mRemainingSubChunkPacketsByColumn.find(pos);
        if (remaining != mRemainingSubChunkPacketsByColumn.end() && remaining->second != 0) return;
    }

    mCenterChunksReady = true;
    auto const elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt);
    size_t const queuedCenterColumns = static_cast<size_t>(
        std::count_if(mCenterChunkPositions.begin(), mCenterChunkPositions.end(), [this](ChunkPos const& pos) {
            return !mReusableSnapshotColumns.contains(pos);
        })
    );
    size_t const queuedOuterColumns = mPendingLevelChunkIndices.size() - queuedCenterColumns;
    getLogger().debug(
        "Replay center ready with {} columns in {:.3f} ms after {} ticks; streaming {} outer columns",
        mCenterChunkPositions.size(),
        elapsed.count(),
        mChunkInjectionTicks,
        queuedOuterColumns
    );
}

bool ReplaySession::finishChunkInjection() {
    updateCenterChunkReadiness();
    bool const applyingSnapshot = mApplyingChunkSnapshot;

    size_t completedLevelChunks;
    size_t retainedReplayChunks;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completedLevelChunks = mCompletedLevelChunkPositions.size();
        retainedReplayChunks = mRetainedReplayChunks.size();
    }
    auto const elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt);
    double injectionP95Ms = 0.0;
    double injectionMaxMs = 0.0;
    if (!mChunkInjectionDurationsMs.empty()) {
        auto sortedDurations = mChunkInjectionDurationsMs;
        std::sort(sortedDurations.begin(), sortedDurations.end());
        size_t const p95Index = (sortedDurations.size() * 95 + 99) / 100 - 1;
        injectionP95Ms        = sortedDurations[p95Index];
        injectionMaxMs        = sortedDurations.back();
    }

    if (applyingSnapshot) {
        mSnapshotChunks        = std::move(mApplyingSnapshotChunks);
        mApplyingChunkSnapshot = false;
    }

    if (applyingSnapshot
        && !flushPendingSnapshotGamePackets(
            true,
            std::numeric_limits<size_t>::max(),
            std::chrono::steady_clock::time_point::max()
        )) {
        mReplayFailed = true;
        return false;
    }
    if (applyingSnapshot && !applyPendingSnapshotLocalPlayer()) {
        mReplayFailed = true;
        return false;
    }

    if (applyingSnapshot) {
        getLogger().debug(
            "Applied replay snapshot in {:.3f} ms after {} ticks with {} reused columns, {} direct and {} native "
            "LevelChunks ({} completed), and {} direct SubChunk packets ({} entries) plus {} native packets ({} "
            "entries); plan {:.3f} ms, injection tick p95 {:.3f} ms, max {:.3f} ms",
            elapsed.count(),
            mChunkInjectionTicks,
            mReusedSnapshotColumns,
            mDirectLevelChunks,
            mInjectedLevelChunks,
            completedLevelChunks,
            mDirectSubChunkPackets,
            mDirectSubChunkEntries,
            mInjectedSubChunkPackets,
            mInjectedSubChunkEntries,
            mChunkPlanPreparationMs,
            injectionP95Ms,
            injectionMaxMs
        );
    } else if (mInjectedLevelChunks != 0 || mDirectLevelChunks != 0) {
        getLogger().debug(
            "Applied replay timeline chunks in {:.3f} ms after {} ticks with {} LevelChunks and {} SubChunk "
            "packets ({} entries); {} replay columns retained",
            elapsed.count(),
            mChunkInjectionTicks,
            mInjectedLevelChunks,
            mInjectedSubChunkPackets,
            mInjectedSubChunkEntries,
            retainedReplayChunks
        );
    } else {
        getLogger().debug(
            "Applied replay timeline SubChunks in {:.3f} ms after {} ticks with {} packets ({} entries)",
            elapsed.count(),
            mChunkInjectionTicks,
            mInjectedSubChunkPackets,
            mInjectedSubChunkEntries
        );
    }

    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mCenterChunkPositions.clear();
    mRemainingSubChunkPacketsByColumn.clear();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
    }
    mPendingLevelChunkCursor    = 0;
    mPendingSubChunkCursor      = 0;
    mChunkInjectionTicks        = 0;
    mChunkInjectionIdleTicks    = 0;
    mChunkInjectionPlanPrepared = false;
    mSnapshotGamePacketPhase =
        applyingSnapshot ? SnapshotGamePacketPhase::WaitingAfterPlayerList : SnapshotGamePacketPhase::StreamingChunks;
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;

    if (!applyingSnapshot) {
        for (auto const& [pos, _] : mPendingSnapshotColumns) mDirtySnapshotColumns.emplace(pos);
        mPendingSnapshotColumns.clear();
        mReusableSnapshotColumns.clear();
        mDirectSnapshotColumns.clear();
        mDirectLevelChunkIndices.clear();
        mInjectedLevelChunks     = 0;
        mInjectedSubChunkPackets = 0;
        mInjectedSubChunkEntries = 0;
        mReusedSnapshotColumns   = 0;
        mDirectLevelChunks       = 0;
        mDirectSubChunkPackets   = 0;
        mDirectSubChunkEntries   = 0;
        mChunkInjectionPending   = false;
        return true;
    }

    return false;
}

void ReplaySession::handleNextTick() {
    if (mIsProcessingSnapshot) {
        throw std::runtime_error("Can't go to next tick while processing snapshot");
    }
    render::commitReplayEntityPoses(static_cast<int64_t>(mCurrentTick) + 1);
    mCurrentTick += 1;
    if (mReplayTime) {
        ++*mReplayTime;
        if (mReplayPlayer) mReplayPlayer->getLevel().setTime(*mReplayTime);
    }
}

void ReplaySession::handleSnapshotContext(PlaybackSnapshotContext const& context) {
    if (!mIsProcessingSnapshot) {
        throw std::runtime_error("Snapshot context appeared outside a replay snapshot");
    }
    if (mReaderIndex >= mSnapshotContexts.size() || context != mSnapshotContexts[mReaderIndex]) {
        throw std::runtime_error("Replay snapshot context changed after preflight");
    }
}

void ReplaySession::handleCreateLocalPlayer(PlaybackBuffer& data) {
    auto const  remaining = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

    if (mIsProcessingSnapshot) {
        if (mPendingSnapshotLocalPlayer) {
            getLogger().error("Replay snapshot {} contains more than one CreateLocalPlayer action", mReaderIndex);
            mReplayFailed = true;
            return;
        }
        mPendingSnapshotLocalPlayer = std::move(payload);
        return;
    }

    if (!applyGamePacket(MinecraftPacketIds::AddPlayer, payload)) mReplayFailed = true;
}

bool ReplaySession::sendRecordedTickPacket() {
    if (!mActive || !mWorldReady || mIsPaused) return false;

    return advanceReplayTick(true);
}

bool ReplaySession::advanceReplayTick(bool stopAtEnd) {
    if (!mActive || !mWorldReady) return false;

    int const startingTick = mCurrentTick;
    while (mActive && mCurrentTick == startingTick) {
        if (mReaderIndex >= mReaders.size()) {
            if (stopAtEnd) {
                mIsPaused                = true;
                mPlaybackTickAccumulator = 0.0f;
                getLogger().info("Replay finished and paused at tick {}", mCurrentTick);
            }
            return false;
        }

        auto& reader = mReaders[mReaderIndex];
        if (!reader->handleNextAction(*this)) {
            ++mReaderIndex;
            if (mReaderIndex >= mReaders.size()) {
                if (stopAtEnd) {
                    mIsPaused                = true;
                    mPlaybackTickAccumulator = 0.0f;
                    getLogger().info("Replay finished and paused at tick {}", mCurrentTick);
                }
                return false;
            }

            auto chunkMeta = mMeta.chunks.begin();
            std::advance(chunkMeta, static_cast<std::ptrdiff_t>(mReaderIndex));
            if (chunkMeta->second.forcePlaySnapshot) {
                applySnapshot(*mReaders[mReaderIndex], false);
            } else {
                mReaders[mReaderIndex]->resetToStart();
            }
            if (mReplayFailed) throw std::runtime_error("Unable to apply a replay action");
            return true;
        }

        if (mPendingReplayDimension) return true;

        if (mReplayFailed) throw std::runtime_error("Unable to apply a replay action");
    }
    return true;
}

void ReplaySession::handleLevelChunkCached(int index) {
    if (index < 0 || static_cast<size_t>(index) >= mChunkPackets.size()) {
        mReplayFailed = true;
        return;
    }
    if (!mChunkInjectionPending) {
        mChunkInjectionTicks     = 0;
        mChunkInjectionIdleTicks = 0;
        mCenterChunksReady       = false;
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    }
    mPendingLevelChunkIndices.emplace_back(index);
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
}

void ReplaySession::handleSubChunkCached(int index) {
    if (index < 0 || static_cast<size_t>(index) >= mChunkPackets.size()) {
        mReplayFailed = true;
        return;
    }
    if (!mChunkInjectionPending) {
        mChunkInjectionTicks     = 0;
        mChunkInjectionIdleTicks = 0;
        mCenterChunksReady       = false;
        mSnapshotGamePacketPhase = SnapshotGamePacketPhase::StreamingChunks;
    }
    mPendingSubChunkIndices.emplace_back(index);
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
}

int ReplaySession::cacheInlineChunkPacket(MinecraftPacketIds packetId, std::string payload) {
    auto& packetIndices =
        packetId == MinecraftPacketIds::FullChunkData ? mInlineLevelChunkPacketIndices : mInlineSubChunkPacketIndices;
    auto const hash    = std::hash<std::string_view>{}(payload);
    auto&      indices = packetIndices[hash];
    for (int index : indices) {
        if (index >= 0 && static_cast<size_t>(index) < mChunkPackets.size()
            && mChunkPackets[static_cast<size_t>(index)] == payload) {
            return index;
        }
    }

    auto const index = static_cast<int>(mChunkPackets.size());
    mChunkPackets.emplace_back(std::move(payload));
    indices.emplace_back(index);
    return index;
}

void ReplaySession::handleConfigurationPacket(PlaybackBuffer& data) {
    if (!mIsProcessingSnapshot) {
        throw std::runtime_error("Configuration packet appeared outside a replay snapshot");
    }

    auto const  packetIdValue = data.getVarInt().value();
    auto const  packetId      = static_cast<MinecraftPacketIds>(packetIdValue);
    auto const  remaining     = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

    auto const semantics = describePacketLifecycle(packetId);
    if (!semantics.isConfiguration()) {
        getLogger().error(
            "Replay contains packet {} with lifecycle {} in a configuration action",
            packetIdValue,
            packetLifecycleName(semantics.lifecycle)
        );
        mReplayFailed = true;
        return;
    }

    auto const applied = mAppliedConfigurationPackets.find(packetIdValue);
    if (!semantics.shouldReplayEverySnapshot() && applied != mAppliedConfigurationPackets.end()
        && applied->second == payload) {
        return;
    }

    if (semantics.lifecycle == PacketLifecycle::PreWorldHandshake) {
        mAppliedConfigurationPackets.insert_or_assign(packetIdValue, std::move(payload));
        return;
    }

    if (!applyGamePacket(packetId, payload)) {
        getLogger().error("Unable to apply replay configuration packet {}", packetIdValue);
        mReplayFailed = true;
        return;
    }
    mAppliedConfigurationPackets.insert_or_assign(packetIdValue, std::move(payload));
}

void ReplaySession::handleGamePacket(PlaybackBuffer& data) {
    auto        packetId  = static_cast<MinecraftPacketIds>(data.getVarInt().value());
    auto const  remaining = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

    if (packetId == MinecraftPacketIds::FullChunkData || packetId == MinecraftPacketIds::SubChunkPacket) {
        int const index = cacheInlineChunkPacket(packetId, std::move(payload));
        if (packetId == MinecraftPacketIds::FullChunkData) handleLevelChunkCached(index);
        else handleSubChunkCached(index);
        return;
    }

    if (mIsProcessingSnapshot) {
        if (packetId == MinecraftPacketIds::DimensionDataPacket || packetId == MinecraftPacketIds::SetTime) {
            if (!applyGamePacket(packetId, payload)) mReplayFailed = true;
            return;
        }
        mPendingSnapshotGamePackets.emplace_back(packetId, std::move(payload));
        return;
    }
    if (!applyGamePacket(packetId, payload)) mReplayFailed = true;
}

void ReplaySession::handleMoveEntities(PlaybackBuffer& data) {
    auto dispatchMovementPacket = [this](std::shared_ptr<Packet>& packet) {
        if (!packet || !mNetworkHandler || !packet->mHandler) {
            mReplayFailed = true;
            return false;
        }
        mInjectingPacket.store(packet.get(), std::memory_order_release);
        InjectionReset reset{mInjectingPacket};
        packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
        return true;
    };

    auto const markerOrCount = data.getVarInt().value();
    if (markerOrCount == 0) {
        auto const version = data.getVarInt().value();
        if (version != 1) throw std::runtime_error("Unsupported precise entity movement version");

        auto const count = data.getVarInt().value();
        if (count < 0) throw std::runtime_error("Entity movement count cannot be negative");

        bool const snapMovement = mSeekTargetTick >= 0 && mSnapMovementDuringSeek;
        for (int index = 0; index < count; ++index) {
            ActorUniqueID const id{data.getVarInt64().value()};
            Vec3 const          position{data.getFloat().value(), data.getFloat().value(), data.getFloat().value()};
            Vec2 const          rotation{data.getFloat().value(), data.getFloat().value()};
            float const         headYaw  = data.getFloat().value();
            float const         bodyYaw  = data.getFloat().value();
            bool const          onGround = data.getBool().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            auto* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor) continue;

            auto const previousPose  = replayRenderPose(*actor);
            auto const renderKey     = replayEntityRenderKey(actor->getLevel(), id);
            auto&      entityContext = actor->getEntityContext();
            auto const currentPose   = render::EntityRenderPose{
                replayRenderPosition(position),
                rotation.x,
                rotation.y,
                headYaw,
                bodyYaw,
            };

            if (renderKey) {
                mEntityRenderKeys.insert_or_assign(id, *renderKey);
                render::queueReplayEntityPose(*renderKey, previousPose, currentPose);
            }

            if (actor->isRiding()) {
                actor->mBuiltInComponents->mActorRotationComponent->mRot = rotation;
                actor->mBuiltInComponents->mActorRotationComponent->mRotPrev =
                    snapMovement ? rotation : Vec2{previousPose.pitch, previousPose.yaw};
                actor->setYHeadRotations(headYaw, snapMovement ? headYaw : previousPose.headYaw);
                if (auto bodyRotation = entityContext.tryGetComponent<MobBodyRotationComponent>()) {
                    bodyRotation->mYBodyRot  = bodyYaw;
                    bodyRotation->mYBodyRotO = snapMovement ? bodyYaw : previousPose.bodyYaw;
                }

                if (onGround) {
                    if (!entityContext.hasComponent<OnGroundFlagComponent>()) {
                        entityContext.getRegistry().emplace<OnGroundFlagComponent>(entityContext.mEntity);
                    }
                } else {
                    entityContext.removeComponent<OnGroundFlagComponent>();
                }
                if (snapMovement) cancelNativeMovementInterpolation(*actor, position, rotation, headYaw);
                continue;
            }

            std::shared_ptr<Packet> packet;
            if (actor->isPlayer()) {
                packet = MinecraftPackets::createPacket(MinecraftPacketIds::MovePlayer);
                if (packet) {
                    auto& move          = static_cast<MovePlayerPacket&>(*packet);
                    move.mPlayerID      = actor->getRuntimeID();
                    move.mPos           = position;
                    move.mRot           = rotation;
                    move.mYHeadRot      = headYaw;
                    move.mResetPosition = snapMovement ? PlayerPositionModeComponent::PositionMode::Teleport
                                                       : PlayerPositionModeComponent::PositionMode::Normal;
                    move.mOnGround      = onGround;
                }
            } else {
                auto quantizeRotation = [](float degrees) {
                    return static_cast<schar>(std::floor(degrees * 256.0f / 360.0f));
                };

                packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
                if (packet) {
                    auto& move         = *static_cast<MoveActorAbsolutePacket&>(*packet).mMoveData;
                    move.mRuntimeId    = actor->getRuntimeID();
                    move.mHeader->mRaw = static_cast<uchar>((onGround ? 1 : 0) | (snapMovement ? 2 : 0));
                    move.mPos          = position;
                    move.mRotX         = quantizeRotation(rotation.x);
                    move.mRotY         = quantizeRotation(rotation.y);
                    move.mRotYHead     = quantizeRotation(headYaw);
                    move.mRotYBody     = quantizeRotation(bodyYaw);
                }
            }

            if (!dispatchMovementPacket(packet)) return;

            // MovePlayerPacket has no body-yaw field, so retain the recorded value after native movement handling.
            if (actor->isPlayer()) {
                if (auto bodyRotation = entityContext.tryGetComponent<MobBodyRotationComponent>()) {
                    bodyRotation->mYBodyRot = bodyYaw;
                    if (snapMovement) bodyRotation->mYBodyRotO = bodyYaw;
                }
            }
            if (snapMovement) cancelNativeMovementInterpolation(*actor, position, rotation, headYaw);
        }
        return;
    }

    auto const count = markerOrCount;
    if (count < 0) throw std::runtime_error("Entity movement count cannot be negative");

    for (int index = 0; index < count; ++index) {
        ActorUniqueID const id{data.getVarInt64().value()};
        bool const          isPlayer = data.getBool().value();
        auto const          x        = data.getFloat().value();
        auto const          y        = data.getFloat().value();
        auto const          z        = data.getFloat().value();

        Vec3 const                             targetPosition{x, y, z};
        render::EntityRenderPose               previousPose{};
        render::EntityRenderPose               currentPose{};
        std::optional<render::EntityRenderKey> renderKey;
        std::shared_ptr<Packet>                packet;
        if (isPlayer) {
            auto const pitch    = data.getFloat().value();
            auto const yaw      = data.getFloat().value();
            auto const headYaw  = data.getFloat().value();
            auto const onGround = data.getBool().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            auto* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor || !actor->isPlayer()) continue;

            previousPose = replayRenderPose(*actor);
            renderKey    = replayEntityRenderKey(actor->getLevel(), id);
            Vec2 const rotation{pitch, yaw};
            currentPose = {
                replayRenderPosition(targetPosition),
                pitch,
                yaw,
                headYaw,
                yaw,
            };

            packet = MinecraftPackets::createPacket(MinecraftPacketIds::MovePlayer);
            if (!packet) {
                mReplayFailed = true;
                return;
            }
            auto& move          = static_cast<MovePlayerPacket&>(*packet);
            move.mPlayerID      = actor->getRuntimeID();
            move.mPos           = targetPosition;
            move.mRot           = rotation;
            move.mYHeadRot      = headYaw;
            move.mResetPosition = PlayerPositionModeComponent::PositionMode::Normal;
            move.mOnGround      = onGround;
        } else {
            auto const header   = data.getByte().value();
            auto const rotX     = data.getByte().value();
            auto const rotY     = data.getByte().value();
            auto const headRotY = data.getByte().value();
            auto const bodyRotY = data.getByte().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            auto* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor || actor->isPlayer()) continue;

            previousPose = replayRenderPose(*actor);
            renderKey    = replayEntityRenderKey(actor->getLevel(), id);
            Vec2 const rotation{decodeRotationByte(rotX), decodeRotationByte(rotY)};
            currentPose = {
                replayRenderPosition(targetPosition),
                rotation.x,
                rotation.y,
                decodeRotationByte(headRotY),
                decodeRotationByte(bodyRotY),
            };

            packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
            if (!packet) {
                mReplayFailed = true;
                return;
            }
            auto& move         = *static_cast<MoveActorAbsolutePacket&>(*packet).mMoveData;
            move.mRuntimeId    = actor->getRuntimeID();
            move.mHeader->mRaw = header;
            move.mPos          = targetPosition;
            move.mRotX         = static_cast<schar>(rotX);
            move.mRotY         = static_cast<schar>(rotY);
            move.mRotYHead     = static_cast<schar>(headRotY);
            move.mRotYBody     = static_cast<schar>(bodyRotY);
        }

        if (!dispatchMovementPacket(packet)) return;
        if (renderKey) {
            mEntityRenderKeys.insert_or_assign(id, *renderKey);
            render::queueReplayEntityPose(*renderKey, previousPose, currentPose);
        }
    }
}

bool ReplaySession::applyPendingSnapshotLocalPlayer() {
    if (!mPendingSnapshotLocalPlayer) {
        getLogger().error("Replay snapshot {} has no pending local player to create", mReaderIndex);
        return false;
    }

    auto payload = std::move(*mPendingSnapshotLocalPlayer);
    mPendingSnapshotLocalPlayer.reset();
    if (!applyGamePacket(MinecraftPacketIds::AddPlayer, payload)) {
        getLogger().error("Unable to apply the CreateLocalPlayer action for replay snapshot {}", mReaderIndex);
        return false;
    }
    return true;
}

bool ReplaySession::applyGamePacket(MinecraftPacketIds packetId, std::string_view payload) {
    // Keep UI packets for a future first-person handler and let the replay world own client chunk publishing.
    if (shouldIgnoreReplayPacket(packetId)) return true;

    auto packet = MinecraftPackets::createPacket(packetId);
    if (!packet || !mNetworkHandler || !packet->mHandler) return false;

    ReadOnlyBinaryStream stream(payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted()) return false;

    if (packetId == MinecraftPacketIds::SetTime) {
        mReplayTime = static_cast<SetTimePacket const&>(*packet).mTime;
    }

    if (packetId == MinecraftPacketIds::PlayerList) {
        auto& playerList = static_cast<PlayerListPacket&>(*packet);
        for (auto& entry : *playerList.mEntries) {
            auto& skinOwner = entry.mSkin->mSkinImpl;
            if (!skinOwner) return false;
            skinOwner->mObject.mIsPrimaryUser = false;
        }
    }

    if (packetId == MinecraftPacketIds::ChangeDimension) {
        if (!mReplayPlayer) return false;
        auto const&        change   = static_cast<ChangeDimensionPacket const&>(*packet);
        auto const&        position = *change.mPos;
        auto const&        rotation = mReplayPlayer->getRotation();
        PlaybackView const view{position.x, position.y, position.z, rotation.y, rotation.x};
        (void)ensureReplayDimension(*change.mDimensionId, view);
        return !mReplayFailed;
    }

    ActorUniqueID  entityId{};
    ActorRuntimeID runtimeId{};
    bool           addsEntity = true;
    switch (packetId) {
    case MinecraftPacketIds::AddActor: {
        auto const& addActor = static_cast<AddActorPacket const&>(*packet);
        entityId             = *addActor.mEntityId;
        runtimeId            = *addActor.mRuntimeId;
        break;
    }
    case MinecraftPacketIds::AddItemActor: {
        auto const& addItem = static_cast<AddItemActorPacket const&>(*packet);
        entityId            = *addItem.mId;
        runtimeId           = *addItem.mRuntimeId;
        break;
    }
    case MinecraftPacketIds::AddPainting: {
        auto const& addPainting = static_cast<AddPaintingPacket const&>(*packet);
        entityId                = *addPainting.mEntityId;
        runtimeId               = *addPainting.mRuntimeId;
        break;
    }
    case MinecraftPacketIds::AddPlayer: {
        auto& addPlayer = static_cast<AddPlayerPacket&>(*packet);
        if (addPlayer.mPlayerGameType == GameType::Default || addPlayer.mPlayerGameType == GameType::Undefined) {
            auto const& abilities = *addPlayer.mAbilities;
            if (abilities.getBool(AbilitiesIndex::NoClip)) {
                addPlayer.mPlayerGameType = GameType::Spectator;
            } else if (abilities.getBool(AbilitiesIndex::Instabuild)) {
                addPlayer.mPlayerGameType = GameType::Creative;
            } else if (abilities.getBool(AbilitiesIndex::Build) || abilities.getBool(AbilitiesIndex::Mine)) {
                addPlayer.mPlayerGameType = GameType::Survival;
            } else {
                addPlayer.mPlayerGameType = GameType::Adventure;
            }
        }
        entityId  = *addPlayer.mEntityId;
        runtimeId = *addPlayer.mRuntimeId;
        break;
    }
    default:
        addsEntity = false;
        break;
    }

    if (addsEntity && mReplayPlayer) {
        auto& level            = mReplayPlayer->getLevel();
        auto* uniqueCollision  = level.fetchEntity(entityId, false);
        auto* runtimeCollision = level.getRuntimeEntity(runtimeId, false);
        if (uniqueCollision || runtimeCollision) {
            getLogger().warn(
                "Skipping conflicting replay entity packet {} (unique={}, runtime={})",
                packet->getName(),
                entityId.rawID,
                runtimeId.rawID
            );
            return true;
        }

        auto runtimeIdManager = level.getActorRuntimeIDManager();
        if (runtimeIdManager->mLastRuntimeID->rawID < runtimeId.rawID) {
            runtimeIdManager->mLastRuntimeID = runtimeId;
        }
    }

    if (!mIsProcessingSnapshot && !mChunkInjectionPending) invalidateSnapshotColumns(*packet);

    mInjectingPacket.store(packet.get(), std::memory_order_release);
    InjectionReset reset{mInjectingPacket};
    packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);

    if (addsEntity) mRecordedEntityIds.emplace(entityId);

    switch (packetId) {
    case MinecraftPacketIds::RemoveActor: {
        auto const id = *static_cast<RemoveActorPacket const&>(*packet).mEntityId;
        mRecordedEntityIds.erase(id);
        if (auto const renderKey = mEntityRenderKeys.find(id); renderKey != mEntityRenderKeys.end()) {
            render::removeReplayEntityPose(renderKey->second);
            mEntityRenderKeys.erase(renderKey);
        }
        break;
    }
    case MinecraftPacketIds::SetDisplayObjective: {
        auto const& objectiveName = *static_cast<SetDisplayObjectivePacket const&>(*packet).mObjectiveName;
        if (!objectiveName.empty()) mReplayObjectiveNames.emplace(objectiveName);
        break;
    }
    case MinecraftPacketIds::RemoveObjective:
        mReplayObjectiveNames.erase(*static_cast<RemoveObjectivePacket const&>(*packet).mObjectiveName);
        break;
    default:
        break;
    }
    return true;
}

void ReplaySession::invalidateSnapshotColumns(Packet const& packet) {
    auto invalidate = [this](BlockPos const& pos) { mDirtySnapshotColumns.emplace(ChunkPos{pos}); };

    switch (packet.getId()) {
    case MinecraftPacketIds::UpdateBlock:
        invalidate(*static_cast<UpdateBlockPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::UpdateBlockSynced:
        invalidate(*static_cast<UpdateBlockSyncedPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::UpdateSubChunkBlocks:
        invalidate(*static_cast<UpdateSubChunkBlocksPacket const&>(packet).mSubChunkBlockPosition);
        break;
    case MinecraftPacketIds::BlockActorData:
        invalidate(*static_cast<BlockActorDataPacket const&>(packet).mPos);
        break;
    case MinecraftPacketIds::TileEvent: {
        auto const& pos    = *static_cast<BlockEventPacket const&>(packet).mPos;
        auto const  column = ChunkPos{pos};
        mDirtySnapshotColumns.emplace(column);
        mDirtySnapshotColumns.emplace(column.x - 1, column.z);
        mDirtySnapshotColumns.emplace(column.x + 1, column.z);
        mDirtySnapshotColumns.emplace(column.x, column.z - 1);
        mDirtySnapshotColumns.emplace(column.x, column.z + 1);
        break;
    }
    default:
        break;
    }
}

bool ReplaySession::flushPendingSnapshotGamePackets(
    bool                                         playerListOnly,
    size_t                                       maxPackets,
    std::chrono::steady_clock::time_point const& deadline
) {
    auto pending = std::move(mPendingSnapshotGamePackets);
    mPendingSnapshotGamePackets.clear();

    size_t applied = 0;
    for (auto& [packetId, payload] : pending) {
        if (playerListOnly != (packetId == MinecraftPacketIds::PlayerList)) {
            mPendingSnapshotGamePackets.emplace_back(packetId, std::move(payload));
            continue;
        }
        if (applied >= maxPackets || (applied != 0 && std::chrono::steady_clock::now() >= deadline)) {
            mPendingSnapshotGamePackets.emplace_back(packetId, std::move(payload));
            continue;
        }

        if (!applyGamePacket(packetId, payload)) return false;
        ++applied;
    }
    if (applied != 0) {
        getLogger().debug(
            "Applied {} replay snapshot game packets in {} phase",
            applied,
            playerListOnly ? "player-list" : "entity"
        );
    }
    return true;
}

bool ReplaySession::clearRecordedEntities() {
    mEntityRenderKeys.clear();
    render::clearReplayEntityPoses();
    if (mRecordedEntityIds.empty()) return true;
    if (!mNetworkHandler) return false;

    auto ids = std::move(mRecordedEntityIds);
    mRecordedEntityIds.clear();
    for (auto const& id : ids) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveActor);
        if (!packet || !packet->mHandler) return false;
        static_cast<RemoveActorPacket&>(*packet).mEntityId = id;

        mInjectingPacket.store(packet.get(), std::memory_order_release);
        InjectionReset reset{mInjectingPacket};
        packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    }
    return true;
}

bool ReplaySession::applyRequestModeLevelChunkDirect(std::string_view payload) {
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
    if (!packet) return false;

    ReadOnlyBinaryStream packetStream(payload, false);
    if (!packet->read(packetStream) || !packetStream.ensureReadCompleted()) return false;

    auto const& levelChunk = static_cast<LevelChunkPacket const&>(*packet);
    if (static_cast<bool>(levelChunk.mCacheEnabled) || !static_cast<bool>(levelChunk.mClientNeedsToRequestSubchunks)
        || static_cast<DimensionType const&>(levelChunk.mDimensionId) != replayDimension->getDimensionId()) {
        return false;
    }

    auto const& pos   = *levelChunk.mPos;
    auto        chunk = replayDimension->getChunkSource().getExistingChunk(pos);
    if (!chunk || chunk->mIsEmptyClientChunk
        || chunk->mLoadState->load(std::memory_order_acquire) != ChunkState::Loaded) {
        return false;
    }

    try {
        ReadOnlyBinaryStream levelStream(*levelChunk.mSerializedChunk, false);
        VarIntDataInput      levelInput(levelStream);
        chunk->deserializeBiomes(levelInput, true);
        chunk->deserializeBorderBlocks(levelInput);
        if (!levelStream.ensureReadCompleted()) return false;
    } catch (std::exception const& exception) {
        getLogger()
            .error("Unable to apply replay LevelChunk data directly at ({}, {}): {}", pos.x, pos.z, exception.what());
        return false;
    } catch (...) {
        getLogger().error("Unable to apply replay LevelChunk data directly at ({}, {})", pos.x, pos.z);
        return false;
    }

    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mCompletedLevelChunkPositions.emplace(pos);
    }
    mChunkCompletionObserved.store(true, std::memory_order_release);
    ++mDirectLevelChunks;
    return true;
}

bool ReplaySession::applySubChunkDirect(std::string_view payload) {
    if (!mNetworkHandler) return false;
    if (!refreshReplayPlayer()) return false;
    auto*       localPlayer     = static_cast<LocalPlayer*>(mReplayPlayer);
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
    if (!packet) return false;

    ReadOnlyBinaryStream stream(payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted()) return false;

    auto const& subChunk = static_cast<SubChunkPacket const&>(*packet);
    if (static_cast<bool>(subChunk.mCacheEnabled)
        || static_cast<DimensionType const&>(subChunk.mDimensionType) != replayDimension->getDimensionId()) {
        return false;
    }

    size_t appliedEntries = 0;
    try {
        localPlayer->getLevel().notifySubChunkRequestManager(subChunk);
        for (auto const& entry : *subChunk.mSubChunkData) {
            if (!isSuccessfulSubChunkResult(static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult))) {
                continue;
            }
            mNetworkHandler
                ->_handleSubChunkData(mNetworkHandler->mServerGuid.get(), subChunk, entry, localPlayer, true);
            ++appliedEntries;
        }
    } catch (std::exception const& exception) {
        getLogger().error("Unable to apply replay SubChunk data directly: {}", exception.what());
        return false;
    } catch (...) {
        getLogger().error("Unable to apply replay SubChunk data directly");
        return false;
    }

    ++mDirectSubChunkPackets;
    mDirectSubChunkEntries += appliedEntries;
    return true;
}

bool ReplaySession::injectChunkPacket(std::string_view payload, MinecraftPacketIds packetId) {
    if (!mNetworkHandler) return false;
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto packet = MinecraftPackets::createPacket(packetId);
    if (!packet) return false;

    ReadOnlyBinaryStream stream(payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) return false;

    size_t subChunkEntries = 0;
    if (packetId == MinecraftPacketIds::FullChunkData) {
        auto& levelChunk = static_cast<LevelChunkPacket&>(*packet);
        if (static_cast<bool>(levelChunk.mCacheEnabled)
            || static_cast<DimensionType const&>(levelChunk.mDimensionId) != replayDimension->getDimensionId()) {
            return false;
        }

        auto const& pos = *levelChunk.mPos;
        if (mApplyingChunkSnapshot) mApplyingSnapshotChunks.emplace(pos);
        else mSnapshotChunks.emplace(pos);
        auto& chunkSource = replayDimension->getChunkSource();
        auto  replayChunk = chunkSource.getExistingChunk(pos);
        if (!replayChunk) {
            replayChunk = chunkSource.getOrLoadChunk(pos, ChunkSource::LoadMode::None, false);
        }
        if (!replayChunk) {
            getLogger()
                .error("Unable to create replay-world LevelChunk at tick {} for ({}, {})", mCurrentTick, pos.x, pos.z);
            return false;
        }
        {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            mRetainedReplayChunks.insert_or_assign(pos, replayChunk);
            mPendingLevelChunks.emplace(pos);
        }
        mChunkInjectionPending = true;
    } else if (packetId == MinecraftPacketIds::SubChunkPacket) {
        auto& subChunk = static_cast<SubChunkPacket&>(*packet);
        if (static_cast<bool>(subChunk.mCacheEnabled)
            || static_cast<DimensionType const&>(subChunk.mDimensionType) != replayDimension->getDimensionId()) {
            return false;
        }
        std::vector<SubChunkPacket::SubChunkPacketData> successfulEntries;
        successfulEntries.reserve(subChunk.mSubChunkData->size());
        for (auto const& entry : *subChunk.mSubChunkData) {
            if (isSuccessfulSubChunkResult(static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult))) {
                successfulEntries.emplace_back(entry);
            }
        }
        *subChunk.mSubChunkData = std::move(successfulEntries);
        subChunkEntries         = subChunk.mSubChunkData->size();
        if (subChunkEntries == 0) return true;
    } else {
        return false;
    }

    mInjectingPacket.store(packet.get(), std::memory_order_release);
    InjectionReset reset{mInjectingPacket};
    packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    if (packetId == MinecraftPacketIds::FullChunkData) {
        ++mInjectedLevelChunks;
    } else {
        ++mInjectedSubChunkPackets;
        mInjectedSubChunkEntries += subChunkEntries;
    }
    return true;
}

bool ReplaySession::clearReplayObjectives() {
    if (mReplayObjectiveNames.empty()) return true;
    if (!mNetworkHandler) return false;

    auto objectiveNames = std::move(mReplayObjectiveNames);
    mReplayObjectiveNames.clear();
    for (auto& objectiveName : objectiveNames) {
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::RemoveObjective);
        if (!packet || !packet->mHandler) return false;
        static_cast<RemoveObjectivePacket&>(*packet).mObjectiveName = std::move(objectiveName);

        mInjectingPacket.store(packet.get(), std::memory_order_release);
        InjectionReset reset{mInjectingPacket};
        packet->mHandler->handle(mNetworkHandler->mServerGuid.get(), *mNetworkHandler, packet);
    }
    return true;
}

bool ReplaySession::isReplayLevel(Level const& level) {
    auto const& session = getInstance();
    return (session.mActive || session.mCleanupState != CleanupState::None) && !session.mReplayLevelId.empty()
        && level.getLevelId() == session.mReplayLevelId;
}

void ReplaySession::configureReplayDimension(DimensionArguments& arguments) const {
    auto profile = mReplayDimensionProfile.load(std::memory_order_acquire);
    if (!profile || arguments.mDerived->mLevel.getLevelId() != profile->levelId) return;

    auto const dimensionId = arguments.mDimId->id;
    auto const range       = profile->heightRanges.find(dimensionId);
    if (range == profile->heightRanges.end()) return;

    arguments.mHeightRange->mMin = static_cast<short>(range->second.minimum);
    arguments.mHeightRange->mMax = static_cast<short>(range->second.maximum);
}

bool ReplaySession::shouldIsolateChunkPackets() const {
    if (!mActive || mReplayLevelId.empty()) return false;

    auto level = ll::service::getMultiPlayerLevel();
    return level && level->getLevelId() == mReplayLevelId;
}

bool ReplaySession::shouldSuppressNativeChunk(ChunkPos const& pos, DimensionType packetDimension) const {
    // Snapshot column coordinates are only meaningful in their owning dimension.
    if (mPendingReplayDimension && *mPendingReplayDimension == packetDimension) return false;
    if (mChunkIsolationDimension && *mChunkIsolationDimension != packetDimension) return false;

    if (!mChunkInjectionPlanPrepared && (!mWorldReady || mApplyingChunkSnapshot)) return true;

    return mSnapshotChunks.contains(pos) || mApplyingSnapshotChunks.contains(pos);
}

void ReplaySession::setMinecraftScreenModel(std::shared_ptr<MinecraftScreenModel> const& screenModel) {
    mScreenModel = screenModel;
}

void ReplaySession::onLevelJoined(Player& player) {
    if (!mActive) return;
    if (!isReplayLevel(player.getLevel())) {
        getLogger().error("The replay world did not open; replay cancelled");
        stop();
        return;
    }

    mReplayWorldJoined = true;
    mReplayPlayer      = &player;
    (void)refreshReplayPlayer();
}

void ReplaySession::onLevelStartJoin() {
    mScreenModel.reset();
    if (mActive && mReplayWorldJoined) {
        stop();
        return;
    }
    clearNetworkContext();
}

void ReplaySession::onLevelExit() {
    if (mReplayLevelId.empty()) return;
    if (mCleanupState == CleanupState::None) {
        if (!mActive || !mReplayWorldJoined) return;
        mCleanupState = CleanupState::ReadyToDelete;
    } else if (mCleanupState == CleanupState::WaitingForExit) {
        mCleanupState = CleanupState::ReadyToDelete;
    } else {
        return;
    }

    clearReplayData();
    mReplayWorldJoined = false;
    mCleanupWaitTicks  = 0;
}

void ReplaySession::onLevelJoinCancelled() {
    if (mReplayLevelId.empty() || mReplayWorldJoined) return;
    if (mCleanupState == CleanupState::None) {
        if (!mActive) return;
        mCleanupState = CleanupState::ReadyToDelete;
    } else if (mCleanupState == CleanupState::WaitingForExit) {
        mCleanupState = CleanupState::ReadyToDelete;
    } else {
        return;
    }

    clearReplayData();
    mCleanupWaitTicks = 0;
}

void ReplaySession::tryFinalizeWorldCleanup() {
    auto client = ll::service::getClientInstance();
    if (!client || !client->isLeaveGameDone() || client->hasLevel() || client->isWorldActive()) return;

    auto& game = client->getMinecraftGame_DEPRECATED();
    if (game.isInServer() || game.getServerInstance()) return;

    auto& cache          = game.getLevelListCache();
    auto  basePathBuffer = cache.getBasePath();
    auto  worldsPath     = std::filesystem::path(basePathBuffer.get());

    if (mCleanupState == CleanupState::None) {
        if (mActive || !mReplayLevelId.empty() || mOrphanReplayWorldsScanned) return;

        std::error_code ec;
        auto            entries = std::filesystem::directory_iterator(worldsPath, ec);
        if (ec) {
            if (mCleanupWaitTicks++ == 0) {
                getLogger().error("Unable to scan for orphaned replay worlds: {}", ec.message());
            }
            return;
        }

        for (auto const& entry : entries) {
            if (!entry.is_directory(ec)) {
                if (ec) break;
                continue;
            }

            auto levelId = entry.path().filename().string();
            if (!isValidReplayLevelId(levelId)) continue;

            getLogger().debug("Found orphaned replay world {}; scheduling removal", levelId);
            mReplayLevelId    = std::move(levelId);
            mCleanupState     = CleanupState::ReadyToDelete;
            mCleanupWaitTicks = 0;
            break;
        }
        if (ec) {
            getLogger().error("Unable to finish scanning for orphaned replay worlds: {}", ec.message());
            return;
        }
        if (mCleanupState == CleanupState::None) {
            mOrphanReplayWorldsScanned = true;
            mCleanupWaitTicks          = 0;
            return;
        }
    }

    if (mCleanupState == CleanupState::WaitingForExit) {
        mCleanupState      = CleanupState::ReadyToDelete;
        mReplayWorldJoined = false;
        mCleanupWaitTicks  = 0;
    }

    if (!isValidReplayLevelId(mReplayLevelId)) {
        if (mCleanupWaitTicks++ == 0) {
            getLogger().error("Refusing to remove invalid replay world id {}", mReplayLevelId);
        }
        return;
    }

    auto worldPath = worldsPath / mReplayLevelId;

    std::error_code ec;
    bool            worldFilesExist = std::filesystem::exists(worldPath, ec);
    if (ec) {
        if (mCleanupWaitTicks++ == 0) {
            getLogger().error("Unable to inspect replay world {}: {}", mReplayLevelId, ec.message());
        }
        return;
    }

    bool levelIsCached = cache.hasLevelWithId(mReplayLevelId);
    if (!levelIsCached && !worldFilesExist) {
        getLogger().debug("Removed replay world {}", mReplayLevelId);
        finishWorldCleanup();
        return;
    }

    if (mCleanupState == CleanupState::ReadyToDelete) {
        getLogger().debug("Removing replay world {}", mReplayLevelId);
        mCleanupState     = CleanupState::DeleteIssued;
        mCleanupWaitTicks = 0;
        try {
            cache.deleteLevel(mReplayLevelId);
        } catch (std::exception const& e) {
            getLogger().error("Unable to remove replay world {}: {}", mReplayLevelId, e.what());
        } catch (...) {
            getLogger().error("Unable to remove replay world {}", mReplayLevelId);
        }
        return;
    }

    ++mCleanupWaitTicks;
    if (mCleanupWaitTicks == REPLAY_WORLD_DELETE_TIMEOUT_TICKS) {
        getLogger().error("Replay world {} still exists after deletion was requested", mReplayLevelId);
    }
}

void ReplaySession::captureNetworkContext(LegacyClientNetworkHandler& handler) {
    if (mNetworkHandler == &handler) return;

    mNetworkHandler = &handler;
}

void ReplaySession::clearNetworkContext() { mNetworkHandler = nullptr; }

void ReplaySession::onLevelChunkHandled(ChunkPos const& pos, Dimension const& dimension) {
    if (mReplayDimension.load(std::memory_order_acquire) != &dimension) return;

    auto       chunk      = dimension.getChunkSource().getExistingChunk(pos);
    auto const loadState  = chunk ? chunk->mLoadState->load(std::memory_order_acquire) : ChunkState::Unloaded;
    bool const retainable = chunk && !chunk->mIsEmptyClientChunk && loadState == ChunkState::Loaded;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        auto             it = mPendingLevelChunks.find(pos);
        if (it == mPendingLevelChunks.end()) return;

        mPendingLevelChunks.erase(it);
        mCompletedLevelChunkPositions.emplace(pos);
        if (retainable) mRetainedReplayChunks.insert_or_assign(pos, chunk);
        else mRetainedReplayChunks.erase(pos);
        mChunkCompletionObserved.store(true, std::memory_order_release);
    }

    if (!retainable) {
        getLogger().warn(
            "Replay LevelChunk completion for ({}, {}) did not leave a loaded chunk in the replay ChunkSource "
            "(existing={}, empty={}, load_state={})",
            pos.x,
            pos.z,
            static_cast<bool>(chunk),
            chunk ? static_cast<bool>(chunk->mIsEmptyClientChunk) : false,
            chunk ? static_cast<int>(loadState) : -1
        );
    }
}

} // namespace playback::functions
