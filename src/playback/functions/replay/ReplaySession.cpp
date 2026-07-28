#include "ReplaySession.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/vanilla_components/OnGroundFlagComponent.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "mc/entity/components/MobBodyRotationComponent.h"
#include "mc/network/IPacketHandlerDispatcher.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetworkIdentifier.h"
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
#include "mc/network/packet/PlayerListPacket.h"
#include "mc/network/packet/RemoveActorPacket.h"
#include "mc/network/packet/SetTimePacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/network/packet/UpdateBlockPacket.h"
#include "mc/network/packet/UpdateBlockSyncedPacket.h"
#include "mc/network/packet/UpdateSubChunkBlocksPacket.h"
#include "mc/util/VarIntDataInput.h"
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

} // namespace

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
        auto const& view = *mMeta.initialView;

        LevelSettings settings;
        settings.mGameType                  = GameType::Spectator;
        settings.mForceGameType             = true;
        settings.mGenerator                 = GeneratorType::Void;
        settings.mImmutableWorld            = true;
        settings.mMultiplayerGameIntent     = false;
        settings.mLANBroadcastIntent        = false;
        settings.mDisablePlayerInteractions = true;
        settings.mDefaultSpawn              = BlockPos(Vec3{view.x, view.y, view.z});

        mReplayLevelId = createReplayLevelId();
        mCleanupState  = CleanupState::None;
        mActive        = true;
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
    mPlaybackSpeed           = 1.0f;
    mPlaybackTickAccumulator = 0.0f;
    mReplayTime.reset();
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
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
    mSnapshotViews.clear();
    mChunkPackets.clear();
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
        mRetainedReplayChunks.clear();
    }
    mPendingLevelChunkIndices.clear();
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mPendingSnapshotGamePackets.clear();
    mRecordedEntityIds.clear();
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

int ReplaySession::getTotalTicks() const {
    return std::max(0, mMeta.totalTicks > 0 ? mMeta.totalTicks : mMeta.duration);
}

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

    size_t selectedReader = mReaders.size() - 1;
    int    selectedStart  = 0;
    int    chunkStart     = 0;
    size_t chunkIndex     = 0;
    for (auto const& [_, chunkMeta] : mMeta.chunks) {
        int const chunkDuration = std::max(0, chunkMeta.duration);
        int const chunkEnd      = chunkStart + chunkDuration;
        if (targetTick < chunkEnd || chunkIndex + 1 == mReaders.size()) {
            selectedReader = chunkIndex;
            selectedStart  = chunkStart;
            break;
        }
        chunkStart = chunkEnd;
        ++chunkIndex;
    }

    mIsPaused                = true;
    mPlaybackTickAccumulator = 0.0f;
    mSeekTargetTick          = targetTick;
    if (targetTick >= mCurrentTick) {
        getLogger().debug(
            "Fast-forwarding replay from tick {} to tick {} without reloading snapshots",
            mCurrentTick,
            targetTick
        );
        return;
    }

    mReaderIndex = selectedReader;
    mCurrentTick = selectedStart;
    applySnapshot(*mReaders[mReaderIndex], false);
    getLogger()
        .debug("Seeking replay to tick {} from snapshot {} at tick {}", targetTick, selectedReader, selectedStart);
}

void ReplaySession::tick() {
    if (!mActive) return;
    if (mReplayTime && mReplayPlayer) mReplayPlayer->getLevel().setTime(*mReplayTime);
    if (mStopRequested.exchange(false, std::memory_order_acq_rel)) {
        stop();
        return;
    }

    try {
        if (!mReplayWorldJoined || !mReplayPlayer || !mNetworkHandler) return;

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
        if (requestedSeek >= 0) beginSeek(requestedSeek);
        if (mChunkInjectionPending) return;

        if (mSeekTargetTick >= 0) {
            int advancedTicks = 0;
            while (mCurrentTick < mSeekTargetTick && !mChunkInjectionPending && advancedTicks < SeekTicksPerClientTick
            ) {
                if (!advanceReplayTick(false)) {
                    getLogger().warn("Replay ended at tick {} while seeking to tick {}", mCurrentTick, mSeekTargetTick);
                    mSeekTargetTick = -1;
                    return;
                }
                ++advancedTicks;
            }
            if (mCurrentTick >= mSeekTargetTick) {
                getLogger().debug("Replay seek completed at tick {}", mCurrentTick);
                mSeekTargetTick = -1;
            }
            return;
        }

        if (mIsPaused) return;
        mPlaybackTickAccumulator += mPlaybackSpeed;
        int const ticksToAdvance  = static_cast<int>(mPlaybackTickAccumulator);
        mPlaybackTickAccumulator -= static_cast<float>(ticksToAdvance);
        for (int tick = 0; tick < ticksToAdvance && !mChunkInjectionPending; ++tick) {
            if (!advanceReplayTick(true)) break;
        }
    } catch (std::exception const& e) {
        getLogger().error("Replay session failed: {}", e.what());
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
    if (!mMeta.initialView) {
        getLogger().error("Replay archive does not contain an initial view");
        return false;
    }

    mReaders.clear();
    mSnapshotViews.clear();
    mChunkPackets.clear();

    bool hasSnapshotView = false;
    bool hasMissingView  = false;
    for (auto const& [chunkName, chunkMeta] : mMeta.chunks) {
        auto chunk = readArchiveEntry(archive.get(), chunkName);
        if (!chunk) {
            getLogger().error("Replay archive does not contain {}", chunkName);
            return false;
        }
        mReaders.emplace_back(std::make_unique<ReplayReader>(*chunk));
        mSnapshotViews.emplace_back(chunkMeta.initialView);
        hasSnapshotView = hasSnapshotView || chunkMeta.initialView.has_value();
        hasMissingView  = hasMissingView || !chunkMeta.initialView.has_value();
    }
    if (hasSnapshotView && hasMissingView) {
        getLogger().error("Replay archive contains only some per-snapshot playback views");
        return false;
    }
    if (!hasSnapshotView) {
        getLogger().warn("Replay archive has no per-snapshot playback views; deriving legacy views from chunk bounds");
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
    mPlaybackSpeed           = 1.0f;
    mPlaybackTickAccumulator = 0.0f;
    mReplayTime.reset();
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
    mAppliedSnapshotColumns.clear();
    mPendingSnapshotColumns.clear();
    mDirtySnapshotColumns.clear();
    mReusableSnapshotColumns.clear();
    mDirectSnapshotColumns.clear();
    mDirectLevelChunkIndices.clear();
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
    mPendingSnapshotGamePackets.clear();
    mRecordedEntityIds.clear();
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

    applySnapshot(*mReaders.front(), true);
}

void ReplaySession::applySnapshot(ReplayReader& reader, bool positionPlayer) {
    if (mChunkInjectionPending) throw std::runtime_error("Previous replay snapshot is still being applied");

    auto resolveReplayPlayer = [this]() -> Player* {
        auto  client = ll::service::getClientInstance();
        auto* player = client ? client->getLocalPlayer() : nullptr;
        if (!player || player != mReplayPlayer)
            throw std::runtime_error("Replay player changed while applying snapshot");
        return player;
    };
    auto* replayPlayer = resolveReplayPlayer();

    mPendingSnapshotGamePackets.clear();
    if (!clearRecordedEntities()) {
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

    PlaybackView view;
    if (positionPlayer) {
        if (mReaderIndex >= mSnapshotViews.size())
            throw std::runtime_error("Replay snapshot view index is out of range");
        auto snapshotView = mSnapshotViews[mReaderIndex];
        if (!snapshotView) snapshotView = deriveLegacySnapshotView();
        if (!snapshotView) throw std::runtime_error("Unable to determine replay snapshot view");
        view = *snapshotView;
        replayPlayer->moveTo(Vec3{view.x, view.y, view.z}, Vec2{view.pitch, view.yaw});
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
        "Prepared replay snapshot {} around ({:.3f}, {:.3f}, {:.3f}) before chunk injection (position player={})",
        mReaderIndex,
        view.x,
        view.y,
        view.z,
        positionPlayer
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
        pending.index      = index;
        auto const& center = *subChunk.mCenterPos;
        for (auto const& entry : entries) {
            auto const result = static_cast<SubChunkPacket::SubChunkRequestResult const&>(entry.mResult);
            if (result != SubChunkPacket::SubChunkRequestResult::Success
                && result != SubChunkPacket::SubChunkRequestResult::SuccessAllAir) {
                getLogger().error(
                    "Replay SubChunk packet {} contains unsuccessful result {}",
                    index,
                    static_cast<int>(result)
                );
                return false;
            }
            auto const&    offset = *entry.mSubChunkPosOffset;
            ChunkPos const target{center.x + static_cast<int>(offset.mX), center.z + static_cast<int>(offset.mZ)};
            subChunkIndicesByColumn[target].emplace(center.y + static_cast<int>(offset.mY));
            if (std::find(pending.targets.begin(), pending.targets.end(), target) == pending.targets.end()) {
                pending.targets.emplace_back(target);
            }
        }
        int64_t priority = std::numeric_limits<int64_t>::max();
        for (auto const& target : pending.targets) {
            if (!levelChunkPositions.contains(target) && !mSnapshotChunks.contains(target)) {
                getLogger().error(
                    "Replay SubChunk packet {} targets column ({}, {}) without a LevelChunk",
                    index,
                    target.x,
                    target.z
                );
                return false;
            }
            targetColumns[target].subChunkIndices.emplace_back(index);
            priority = std::min(priority, distanceSquared(target));
        }
        subChunks.push_back(PrioritizedSubChunk{std::move(pending), priority});
    }

    if (mApplyingChunkSnapshot) {
        for (auto const& pos : requestModeLevelChunks) {
            auto target = targetColumns.find(pos);
            if (target == targetColumns.end() || target->second.subChunkIndices.empty()) {
                getLogger().error(
                    "Replay request-mode LevelChunk column ({}, {}) has no successful SubChunk packet",
                    pos.x,
                    pos.z
                );
                return false;
            }
        }
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

    if (targetColumns.empty() && mApplyingChunkSnapshot) {
        getLogger().error("Replay snapshot contains no chunk packets");
        return false;
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
        auto const&        position = player->getPosition();
        auto const&        rotation = player->getRotation();
        PlaybackView const view{position.x, position.y, position.z, rotation.y, rotation.x};
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
            applied = applyLevelChunkDirect(mChunkPackets[static_cast<size_t>(index)]);
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
        bool const direct  = std::all_of(pending.targets.begin(), pending.targets.end(), [this](ChunkPos const& pos) {
            return mDirectSnapshotColumns.contains(pos) || mReusableSnapshotColumns.contains(pos);
        });
        bool       applied = direct ? applySubChunkDirect(mChunkPackets[static_cast<size_t>(pending.index)])
                                    : injectChunkPacket(
                                    mChunkPackets[static_cast<size_t>(pending.index)],
                                    MinecraftPacketIds::SubChunkPacket
                                );
        if (direct && !applied) {
            getLogger().warn("Direct replay SubChunk update became unavailable; falling back to native loading");
            for (auto const& target : pending.targets) mDirectSnapshotColumns.erase(target);
            applied = injectChunkPacket(
                mChunkPackets[static_cast<size_t>(pending.index)],
                MinecraftPacketIds::SubChunkPacket
            );
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
    size_t const queuedCenterColumns = static_cast<size_t>(std::count_if(
        mCenterChunkPositions.begin(),
        mCenterChunkPositions.end(),
        [this](ChunkPos const& pos) { return !mReusableSnapshotColumns.contains(pos); }
    ));
    size_t const queuedOuterColumns  = mPendingLevelChunkIndices.size() - queuedCenterColumns;
    getLogger().debug(
        "Replay center ready with {} columns in {:.3f} ms after {} ticks; streaming {} outer columns",
        mCenterChunkPositions.size(),
        elapsed.count(),
        mChunkInjectionTicks,
        queuedOuterColumns
    );
}

std::optional<PlaybackView> ReplaySession::deriveLegacySnapshotView() const {
    if (!mMeta.initialView || mPendingLevelChunkIndices.empty()) return std::nullopt;

    int minX = std::numeric_limits<int>::max();
    int minZ = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxZ = std::numeric_limits<int>::min();

    for (int index : mPendingLevelChunkIndices) {
        if (index < 0 || static_cast<size_t>(index) >= mChunkPackets.size()) return std::nullopt;

        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::FullChunkData);
        if (!packet) return std::nullopt;

        ReadOnlyBinaryStream stream(mChunkPackets[static_cast<size_t>(index)], false);
        if (!packet->read(stream) || !stream.ensureReadCompleted() || !packet->mHandler) return std::nullopt;

        auto const& pos = *static_cast<LevelChunkPacket const&>(*packet).mPos;
        minX            = std::min(minX, pos.x);
        minZ            = std::min(minZ, pos.z);
        maxX            = std::max(maxX, pos.x);
        maxZ            = std::max(maxZ, pos.z);
    }

    auto view = *mMeta.initialView;
    view.x    = static_cast<float>(minX + maxX + 1) * 8.0f;
    view.z    = static_cast<float>(minZ + maxZ + 1) * 8.0f;
    getLogger().warn(
        "Derived legacy replay snapshot {} view from chunk bounds ({}, {}) to ({}, {})",
        mReaderIndex,
        minX,
        minZ,
        maxX,
        maxZ
    );
    return view;
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
    // TODO: Flash pending entities

    mCurrentTick += 1;
    if (mReplayTime) {
        ++*mReplayTime;
        if (mReplayPlayer) mReplayPlayer->getLevel().setTime(*mReplayTime);
    }
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

            if (stopAtEnd) {
                applySnapshot(*mReaders[mReaderIndex], false);
            } else {
                mReaders[mReaderIndex]->resetToStart();
            }
            if (mReplayFailed) throw std::runtime_error("Unable to apply a replay action");
            return true;
        }

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

void ReplaySession::handleGamePacket(PlaybackBuffer& data) {
    auto        packetId  = static_cast<MinecraftPacketIds>(data.getVarInt().value());
    auto const  remaining = data.getWritePointer() - data.mReadPointer;
    std::string payload(data.mView.data() + data.mReadPointer, remaining);
    data.mReadPointer += remaining;

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

        bool const snapMovement = mSeekTargetTick >= 0;
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

            auto const previousRotation = actor->getRotation();
            auto&      entityContext    = actor->getEntityContext();

            if (actor->isRiding()) {
                float previousHeadYaw = previousRotation.y;
                if (auto headRotation = entityContext.tryGetComponent<ActorHeadRotationComponent>()) {
                    previousHeadYaw = headRotation->mYHeadRot;
                }

                float previousBodyYaw = previousRotation.y;
                if (auto bodyRotation = entityContext.tryGetComponent<MobBodyRotationComponent>()) {
                    previousBodyYaw = bodyRotation->mYBodyRot;
                }

                actor->mBuiltInComponents->mActorRotationComponent->mRot = rotation;
                actor->mBuiltInComponents->mActorRotationComponent->mRotPrev =
                    snapMovement ? rotation : previousRotation;
                actor->setYHeadRotations(headYaw, snapMovement ? headYaw : previousHeadYaw);
                if (auto bodyRotation = entityContext.tryGetComponent<MobBodyRotationComponent>()) {
                    bodyRotation->mYBodyRot  = bodyYaw;
                    bodyRotation->mYBodyRotO = snapMovement ? bodyYaw : previousBodyYaw;
                }

                if (onGround) {
                    if (!entityContext.hasComponent<OnGroundFlagComponent>()) {
                        entityContext.getRegistry().emplace<OnGroundFlagComponent>(entityContext.mEntity);
                    }
                } else {
                    entityContext.removeComponent<OnGroundFlagComponent>();
                }
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

        std::shared_ptr<Packet> packet;
        if (isPlayer) {
            auto const pitch    = data.getFloat().value();
            auto const yaw      = data.getFloat().value();
            auto const headYaw  = data.getFloat().value();
            auto const onGround = data.getBool().value();

            if (!mRecordedEntityIds.contains(id) || !mReplayPlayer) continue;
            auto* actor = mReplayPlayer->getLevel().fetchEntity(id, false);
            if (!actor || !actor->isPlayer()) continue;

            packet = MinecraftPackets::createPacket(MinecraftPacketIds::MovePlayer);
            if (!packet) {
                mReplayFailed = true;
                return;
            }
            auto& move          = static_cast<MovePlayerPacket&>(*packet);
            move.mPlayerID      = actor->getRuntimeID();
            move.mPos           = Vec3{x, y, z};
            move.mRot           = Vec2{pitch, yaw};
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

            packet = MinecraftPackets::createPacket(MinecraftPacketIds::MoveAbsoluteActor);
            if (!packet) {
                mReplayFailed = true;
                return;
            }
            auto& move         = *static_cast<MoveActorAbsolutePacket&>(*packet).mMoveData;
            move.mRuntimeId    = actor->getRuntimeID();
            move.mHeader->mRaw = header;
            move.mPos          = Vec3{x, y, z};
            move.mRotX         = static_cast<schar>(rotX);
            move.mRotY         = static_cast<schar>(rotY);
            move.mRotYHead     = static_cast<schar>(headRotY);
            move.mRotYBody     = static_cast<schar>(bodyRotY);
        }

        if (!dispatchMovementPacket(packet)) return;
    }
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

    DimensionType changedDimension{};
    if (packetId == MinecraftPacketIds::ChangeDimension) {
        if (!mReplayPlayer) return false;
        changedDimension = *static_cast<ChangeDimensionPacket const&>(*packet).mDimensionId;
        auto dimension   = mReplayPlayer->getLevel().getOrCreateDimension(changedDimension).lock();
        if (!dimension) return false;
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

    switch (packetId) {
    case MinecraftPacketIds::AddActor:
        mRecordedEntityIds.emplace(*static_cast<AddActorPacket const&>(*packet).mEntityId);
        break;
    case MinecraftPacketIds::AddItemActor:
        mRecordedEntityIds.emplace(*static_cast<AddItemActorPacket const&>(*packet).mId);
        break;
    case MinecraftPacketIds::AddPainting:
        mRecordedEntityIds.emplace(*static_cast<AddPaintingPacket const&>(*packet).mEntityId);
        break;
    case MinecraftPacketIds::AddPlayer:
        mRecordedEntityIds.emplace(*static_cast<AddPlayerPacket const&>(*packet).mEntityId);
        break;
    case MinecraftPacketIds::RemoveActor:
        mRecordedEntityIds.erase(*static_cast<RemoveActorPacket const&>(*packet).mEntityId);
        break;
    case MinecraftPacketIds::ChangeDimension: {
        auto dimension = mReplayPlayer->getLevel().getOrCreateDimension(changedDimension).lock();
        if (!dimension) return false;

        mReplayDimension.store(dimension.get(), std::memory_order_release);
        mChunkInjectionPending      = false;
        mChunkInjectionPlanPrepared = false;
        mApplyingChunkSnapshot      = false;
        mCenterChunksReady          = false;
        mSnapshotGamePacketPhase    = SnapshotGamePacketPhase::StreamingChunks;
        mPendingLevelChunkIndices.clear();
        mPendingSubChunkIndices.clear();
        mPendingSubChunkPackets.clear();
        mRemainingSubChunkPacketsByColumn.clear();
        mSnapshotChunks.clear();
        mApplyingSnapshotChunks.clear();
        mAppliedSnapshotColumns.clear();
        mPendingSnapshotColumns.clear();
        mDirtySnapshotColumns.clear();
        mReusableSnapshotColumns.clear();
        mDirectSnapshotColumns.clear();
        mDirectLevelChunkIndices.clear();
        mCenterChunkPositions.clear();
        {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            mPendingLevelChunks.clear();
            mCompletedLevelChunkPositions.clear();
            mRetainedReplayChunks.clear();
        }
        break;
    }
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
    getLogger().debug("Cleared {} recorded entities before applying a replay snapshot", ids.size());
    return true;
}

bool ReplaySession::applyLevelChunkDirect(std::string_view payload) {
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
    auto const* replayDimension = mReplayDimension.load(std::memory_order_acquire);
    if (!replayDimension) return false;

    auto  client      = ll::service::getClientInstance();
    auto* localPlayer = client ? client->getLocalPlayer() : nullptr;
    if (!localPlayer || localPlayer != mReplayPlayer) return false;

    auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::SubChunkPacket);
    if (!packet) return false;

    ReadOnlyBinaryStream stream(payload, false);
    if (!packet->read(stream) || !stream.ensureReadCompleted()) return false;

    auto const& subChunk = static_cast<SubChunkPacket const&>(*packet);
    if (static_cast<bool>(subChunk.mCacheEnabled)
        || static_cast<DimensionType const&>(subChunk.mDimensionType) != replayDimension->getDimensionId()) {
        return false;
    }

    try {
        for (auto const& entry : *subChunk.mSubChunkData) {
            mNetworkHandler
                ->_handleSubChunkData(mNetworkHandler->mServerGuid.get(), subChunk, entry, localPlayer, true);
        }
    } catch (std::exception const& exception) {
        getLogger().error("Unable to apply replay SubChunk data directly: {}", exception.what());
        return false;
    } catch (...) {
        getLogger().error("Unable to apply replay SubChunk data directly");
        return false;
    }

    ++mDirectSubChunkPackets;
    mDirectSubChunkEntries += subChunk.mSubChunkData->size();
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
        subChunkEntries = subChunk.mSubChunkData->size();
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

bool ReplaySession::isReplayLevel(Level const& level) {
    auto const& session = getInstance();
    return (session.mActive || session.mCleanupState != CleanupState::None) && !session.mReplayLevelId.empty()
        && level.getLevelId() == session.mReplayLevelId;
}

bool ReplaySession::shouldIsolateChunkPackets() const {
    if (!mActive || mReplayLevelId.empty()) return false;

    auto level = ll::service::getMultiPlayerLevel();
    return level && level->getLevelId() == mReplayLevelId;
}

bool ReplaySession::shouldSuppressNativeChunk(ChunkPos const& pos) const {
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

    mReplayPlayer = &player;
    mReplayDimension.store(&player.getDimension(), std::memory_order_release);
    mReplayWorldJoined = true;
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
