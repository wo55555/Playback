#include "ReplaySession.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/screens/models/MinecraftScreenModel.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/threading/TaskGroup.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/IPacketHandlerDispatcher.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/network/packet/SubChunkPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/LevelSettings.h"
#include "mc/world/level/chunk/ChunkViewSource.h"
#include "mc/world/level/dimension/Dimension.h"
#include "mc/world/level/storage/ILevelListCache.h"

#include "snappy.h"
#include "uuid.h"
#include "zip.h"

#include <algorithm>
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
    mActive       = false;
    mIsPaused     = false;
    mWorldReady   = false;
    mReplayFailed = false;
    mInjectingPacket.store(nullptr, std::memory_order_release);
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mReplayDimension.store(nullptr, std::memory_order_release);
    mInitialSnapshotApplied  = false;
    mChunkInjectionPending   = false;
    mIsProcessingSnapshot    = false;
    mCurrentTick             = 0;
    mReaderIndex             = 0;
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
    mInjectedLevelChunks     = 0;
    mInjectedSubChunkPackets = 0;
    mInjectedSubChunkEntries = 0;
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
    }
    mPendingLevelChunkIndices.clear();
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
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
    getLogger().info("Replay {} at tick {}", paused ? "paused" : "playing", mCurrentTick);
    return true;
}

void ReplaySession::tick() {
    if (!mActive) return;

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
        sendRecordedTickPacket();
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

    mReplayFilePath    = std::move(filePath);
    mCurrentTick       = 0;
    mReaderIndex       = 0;
    mReplayWorldJoined = false;
    mWorldReady        = false;
    mReplayFailed      = false;
    mIsPaused          = true;
    mInjectingPacket.store(nullptr, std::memory_order_release);
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mReplayDimension.store(nullptr, std::memory_order_release);
    mInitialSnapshotApplied  = false;
    mChunkInjectionPending   = false;
    mChunkInjectionTicks     = 0;
    mChunkInjectionIdleTicks = 0;
    mPendingLevelChunkCursor = 0;
    mPendingSubChunkCursor   = 0;
    mReplayPlayer            = nullptr;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        mPendingLevelChunks.clear();
        mCompletedLevelChunkPositions.clear();
    }
    mSnapshotChunks.clear();
    mApplyingSnapshotChunks.clear();
    mPendingLevelChunkIndices.clear();
    mPendingSubChunkIndices.clear();
    mPendingSubChunkPackets.clear();
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

    applySnapshot(*mReaders.front());
}

void ReplaySession::applySnapshot(ReplayReader& reader) {
    if (mChunkInjectionPending) throw std::runtime_error("Previous replay snapshot is still being applied");

    auto resolveReplayPlayer = [this]() -> Player* {
        auto  client = ll::service::getClientInstance();
        auto* player = client ? client->getLocalPlayer() : nullptr;
        if (!player || player != mReplayPlayer)
            throw std::runtime_error("Replay player changed while applying snapshot");
        return player;
    };
    auto* replayPlayer          = resolveReplayPlayer();
    auto  drainReplayChunkTasks = [&](std::string_view phase) {
        replayPlayer    = resolveReplayPlayer();
        auto* dimension = &replayPlayer->getDimension();
        auto* taskGroup = dimension->mTaskGroup.get();
        if (!taskGroup) throw std::runtime_error("Replay dimension has no chunk task group");

        auto started     = std::chrono::steady_clock::now();
        auto beforeState = static_cast<int>(taskGroup->getState());
        auto beforeCount = taskGroup->count();
        taskGroup->sync_DEPRECATED_ASK_TOMMO([] {});

        replayPlayer           = resolveReplayPlayer();
        auto* currentDimension = &replayPlayer->getDimension();
        auto* currentTaskGroup = currentDimension->mTaskGroup.get();
        if (currentDimension != dimension || currentTaskGroup != taskGroup) {
            throw std::runtime_error("Replay dimension chunk task group changed while draining");
        }

        auto afterState = static_cast<int>(currentTaskGroup->getState());
        auto afterCount = currentTaskGroup->count();
        if (!currentTaskGroup->isEmpty()) {
            throw std::runtime_error("Replay dimension chunk task group did not drain");
        }

        auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        getLogger().debug(
            "Drained replay chunk tasks {} in {:.3f} ms (state {} -> {}, count {} -> {})",
            phase,
            elapsedMs,
            beforeState,
            afterState,
            beforeCount,
            afterCount
        );
    };

    if (!mSnapshotChunks.empty()) {
        drainReplayChunkTasks("before snapshot clear");

        {
            auto chunkSource = replayPlayer->mChunkSource;
            if (!chunkSource) throw std::runtime_error("Replay player has no chunk view");
            for (auto const& pos : mSnapshotChunks) chunkSource->clearEntryAtChunkPos(pos);
        }
        mSnapshotChunks.clear();

        drainReplayChunkTasks("after snapshot clear");
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
    mChunkCompletionObserved.store(false, std::memory_order_release);
    mChunkInjectionPlanPrepared = false;
    mCenterChunksReady          = false;
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
    mApplyingSnapshotChunks.clear();
    mInjectedLevelChunks     = 0;
    mInjectedSubChunkPackets = 0;
    mInjectedSubChunkEntries = 0;

    reader.handleSnapshot(*this);
    reader.resetToStart();
    if (mReplayFailed) {
        mApplyingChunkSnapshot = false;
        return;
    }

    if (mReaderIndex >= mSnapshotViews.size()) throw std::runtime_error("Replay snapshot view index is out of range");
    auto snapshotView = mSnapshotViews[mReaderIndex];
    if (!snapshotView) snapshotView = deriveLegacySnapshotView();
    if (!snapshotView) throw std::runtime_error("Unable to determine replay snapshot view");

    auto const& view = *snapshotView;
    replayPlayer->moveTo(Vec3{view.x, view.y, view.z}, Vec2{view.pitch, view.yaw});
    mChunkInjectionStartedAt = std::chrono::steady_clock::now();
    if (!prepareChunkInjectionPlan(view)) {
        mReplayFailed          = true;
        mApplyingChunkSnapshot = false;
        return;
    }
    mChunkPlanPreparationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mChunkInjectionStartedAt).count();
    getLogger().debug(
        "Positioned replay snapshot {} at ({:.3f}, {:.3f}, {:.3f}) before chunk injection",
        mReaderIndex,
        view.x,
        view.y,
        view.z
    );

    mChunkInjectionPending = true;
    getLogger().info(
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

    std::vector<PrioritizedLevelChunk> levelChunks;
    std::unordered_set<ChunkPos>       levelChunkPositions;
    std::unordered_set<ChunkPos>       requestModeLevelChunks;
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
            getLogger().error("Replay LevelChunk packet {} unexpectedly uses the blob cache", index);
            return false;
        }

        ChunkPos const pos = *levelChunk.mPos;
        if (!levelChunkPositions.emplace(pos).second) {
            getLogger().error("Replay snapshot contains duplicate LevelChunk column ({}, {})", pos.x, pos.z);
            return false;
        }
        if (static_cast<bool>(levelChunk.mClientNeedsToRequestSubchunks)) {
            requestModeLevelChunks.emplace(pos);
        }
        levelChunks.push_back(PrioritizedLevelChunk{pos, distanceSquared(pos), index});
    }

    std::stable_sort(levelChunks.begin(), levelChunks.end(), [](auto const& left, auto const& right) {
        if (left.distanceSquared != right.distanceSquared) return left.distanceSquared < right.distanceSquared;
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        return left.pos.z < right.pos.z;
    });

    mPendingLevelChunkIndices.clear();
    mPendingLevelChunkIndices.reserve(levelChunks.size());
    mCenterChunkPositions.clear();
    for (auto const& levelChunk : levelChunks) {
        mPendingLevelChunkIndices.emplace_back(levelChunk.index);
        if (std::abs(levelChunk.pos.x - centerX) <= 2 && std::abs(levelChunk.pos.z - centerZ) <= 2) {
            mCenterChunkPositions.emplace(levelChunk.pos);
        }
    }
    if (mCenterChunkPositions.empty() && !levelChunks.empty()) {
        mCenterChunkPositions.emplace(levelChunks.front().pos);
    }

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
            getLogger().error("Replay SubChunk packet {} unexpectedly uses the blob cache", index);
            return false;
        }

        auto const& entries = *subChunk.mSubChunkData;
        if (entries.empty()) {
            getLogger().error("Replay SubChunk packet {} contains no successful entries", index);
            return false;
        }
        if (entries.size() > SUB_CHUNK_ENTRIES_PER_TICK) {
            getLogger().error(
                "Replay SubChunk packet {} has {} entries, exceeding the per-tick limit {}",
                index,
                entries.size(),
                SUB_CHUNK_ENTRIES_PER_TICK
            );
            return false;
        }

        PendingSubChunkPacket pending;
        pending.index      = index;
        pending.entryCount = entries.size();
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
            if (std::find(pending.targets.begin(), pending.targets.end(), target) == pending.targets.end()) {
                pending.targets.emplace_back(target);
            }
        }
        int64_t priority = std::numeric_limits<int64_t>::max();
        for (auto const& target : pending.targets) {
            bool const currentLevelChunk = levelChunkPositions.contains(target);
            if (!currentLevelChunk && !mSnapshotChunks.contains(target)) {
                getLogger().error(
                    "Replay SubChunk packet {} targets column ({}, {}) without a LevelChunk",
                    index,
                    target.x,
                    target.z
                );
                return false;
            }
            if (currentLevelChunk) pending.dependencies.emplace_back(target);
            ++mRemainingSubChunkPacketsByColumn[target];
            priority = std::min(priority, distanceSquared(target));
        }
        subChunks.push_back(PrioritizedSubChunk{std::move(pending), priority});
    }

    for (auto const& pos : requestModeLevelChunks) {
        if (!mRemainingSubChunkPacketsByColumn.contains(pos)) {
            getLogger().error(
                "Replay request-mode LevelChunk column ({}, {}) has no successful SubChunk packet",
                pos.x,
                pos.z
            );
            return false;
        }
    }

    std::stable_sort(subChunks.begin(), subChunks.end(), [](auto const& left, auto const& right) {
        return left.distanceSquared < right.distanceSquared;
    });
    mPendingSubChunkPackets.clear();
    mPendingSubChunkPackets.reserve(subChunks.size());
    for (auto& subChunk : subChunks) mPendingSubChunkPackets.emplace_back(std::move(subChunk.packet));
    mPendingSubChunkIndices.clear();

    if (mPendingLevelChunkIndices.empty() && mPendingSubChunkPackets.empty()) {
        getLogger().error("Replay snapshot contains no chunk packets");
        return false;
    }

    mPendingLevelChunkCursor    = 0;
    mPendingSubChunkCursor      = 0;
    mChunkInjectionPlanPrepared = true;
    return true;
}

bool ReplaySession::tryFinishChunkInjection() {
    if (!mChunkInjectionPending) return true;
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
    size_t injectedSubChunkEntries = 0;
    if (!injectReadySubChunkPackets(injectedSubChunkPackets, injectedSubChunkEntries, deadline)
        || !injectPendingLevelChunks(deadline)
        || !injectReadySubChunkPackets(injectedSubChunkPackets, injectedSubChunkEntries, deadline)) {
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
    size_t injected = 0;
    while (mPendingLevelChunkCursor < mPendingLevelChunkIndices.size() && injected < LEVEL_CHUNK_PACKETS_PER_TICK) {
        if (injected != 0 && std::chrono::steady_clock::now() >= deadline) break;
        {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            if (mPendingLevelChunks.size() >= MAX_LEVEL_CHUNKS_IN_FLIGHT) break;
        }

        int const index = mPendingLevelChunkIndices[mPendingLevelChunkCursor];
        if (!injectChunkPacket(mChunkPackets[static_cast<size_t>(index)], MinecraftPacketIds::FullChunkData)) {
            getLogger().error(
                "Unable to inject replay LevelChunk packet {} of {}",
                mPendingLevelChunkCursor,
                mPendingLevelChunkIndices.size()
            );
            mReplayFailed = true;
            return false;
        }
        ++mPendingLevelChunkCursor;
        ++injected;
    }
    return true;
}

bool ReplaySession::injectReadySubChunkPackets(
    size_t&                               injectedPackets,
    size_t&                               injectedEntries,
    std::chrono::steady_clock::time_point deadline
) {
    std::unordered_set<ChunkPos> completed;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completed = mCompletedLevelChunkPositions;
    }

    for (auto& pending : mPendingSubChunkPackets) {
        if (pending.injected) continue;
        if (injectedPackets >= SUB_CHUNK_PACKETS_PER_TICK) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (!std::all_of(pending.dependencies.begin(), pending.dependencies.end(), [&completed](ChunkPos const& pos) {
                return completed.contains(pos);
            })) {
            continue;
        }
        if (injectedPackets != 0 && injectedEntries + pending.entryCount > SUB_CHUNK_ENTRIES_PER_TICK) continue;

        if (!injectChunkPacket(mChunkPackets[static_cast<size_t>(pending.index)], MinecraftPacketIds::SubChunkPacket)) {
            getLogger().error("Unable to inject replay SubChunk packet {}", pending.index);
            mReplayFailed = true;
            return false;
        }

        pending.injected = true;
        ++mPendingSubChunkCursor;
        ++injectedPackets;
        injectedEntries += pending.entryCount;
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
    getLogger().info(
        "Replay center ready with {} columns in {:.3f} ms after {} ticks; streaming {} outer columns",
        mCenterChunkPositions.size(),
        elapsed.count(),
        mChunkInjectionTicks,
        mPendingLevelChunkIndices.size() - mCenterChunkPositions.size()
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

    size_t completedLevelChunks;
    {
        std::scoped_lock lock(mPendingLevelChunksMutex);
        completedLevelChunks = mCompletedLevelChunkPositions.size();
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

    if (mApplyingChunkSnapshot) {
        mSnapshotChunks        = std::move(mApplyingSnapshotChunks);
        mApplyingChunkSnapshot = false;
    }

    getLogger().info(
        "Applied replay snapshot in {:.3f} ms after {} ticks with {} LevelChunk packets ({} completed) and {} "
        "SubChunk packets ({} entries); plan {:.3f} ms, injection tick p95 {:.3f} ms, max {:.3f} ms",
        elapsed.count(),
        mChunkInjectionTicks,
        mInjectedLevelChunks,
        completedLevelChunks,
        mInjectedSubChunkPackets,
        mInjectedSubChunkEntries,
        mChunkPlanPreparationMs,
        injectionP95Ms,
        injectionMaxMs
    );

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
    mChunkInjectionPending      = false;
    mChunkInjectionPlanPrepared = false;
    mChunkInjectionDurationsMs.clear();
    mChunkPlanPreparationMs = 0.0;
    return true;
}

void ReplaySession::handleNextTick() {
    if (mIsProcessingSnapshot) {
        throw std::runtime_error("Can't go to next tick while processing snapshot");
    }
    // TODO: Flash pending entities

    mCurrentTick += 1;
}

bool ReplaySession::sendRecordedTickPacket() {
    if (!mActive || !mWorldReady || mIsPaused) return false;

    int const startingTick = mCurrentTick;
    while (mActive && mCurrentTick == startingTick) {
        if (mReaderIndex >= mReaders.size()) {
            getLogger().info("Replay finished at tick {}", mCurrentTick);
            stop();
            return false;
        }

        auto& reader = mReaders[mReaderIndex];
        if (!reader->handleNextAction(*this)) {
            ++mReaderIndex;
            if (mReaderIndex >= mReaders.size()) {
                getLogger().info("Replay finished at tick {}", mCurrentTick);
                stop();
                return false;
            }

            applySnapshot(*mReaders[mReaderIndex]);
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
    mPendingLevelChunkIndices.emplace_back(index);
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
}

void ReplaySession::handleSubChunkCached(int index) {
    if (index < 0 || static_cast<size_t>(index) >= mChunkPackets.size()) {
        mReplayFailed = true;
        return;
    }
    mPendingSubChunkIndices.emplace_back(index);
    mChunkInjectionPending      = true;
    mChunkInjectionPlanPrepared = false;
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

        if (mApplyingChunkSnapshot) mApplyingSnapshotChunks.emplace(*levelChunk.mPos);
        else mSnapshotChunks.emplace(*levelChunk.mPos);
        {
            std::scoped_lock lock(mPendingLevelChunksMutex);
            mPendingLevelChunks.emplace(*levelChunk.mPos);
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
    if (mCleanupState == CleanupState::None) return;

    auto client = ll::service::getClientInstance();
    if (!client || !client->isLeaveGameDone() || client->hasLevel() || client->isWorldActive()) return;

    auto& game = client->getMinecraftGame_DEPRECATED();
    if (game.isInServer() || game.getServerInstance()) return;

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

    auto& cache          = game.getLevelListCache();
    auto  basePathBuffer = cache.getBasePath();
    auto  worldPath      = std::filesystem::path(basePathBuffer.get()) / mReplayLevelId;

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
        getLogger().info("Removed replay world {}", mReplayLevelId);
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

    std::scoped_lock lock(mPendingLevelChunksMutex);
    auto             it = mPendingLevelChunks.find(pos);
    if (it != mPendingLevelChunks.end()) {
        mPendingLevelChunks.erase(it);
        mCompletedLevelChunkPositions.emplace(pos);
        mChunkCompletionObserved.store(true, std::memory_order_release);
    }
}

} // namespace playback::functions
