#pragma once

#include "playback/functions/record/Recorder.h"

#include "mc/legacy/ActorUniqueID.h"
#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Level;
class Dimension;
class LevelChunk;
class LegacyClientNetworkHandler;
class MinecraftScreenModel;
class Player;
enum class MinecraftPacketIds : int;

namespace playback::functions {

class ReplaySession {
private:
    static constexpr size_t MAX_LEVEL_CHUNKS_IN_FLIGHT           = 64;
    static constexpr size_t MAX_SUB_CHUNK_ENTRIES_PER_PACKET     = 1536;
    static constexpr size_t SNAPSHOT_GAME_PACKETS_PER_TICK       = 16;
    static constexpr int    CHUNK_INJECTION_STALL_TIMEOUT_TICKS  = 20 * 30;
    static constexpr int    DIMENSION_TRANSITION_SETTLE_UPDATES  = 2;
    static constexpr int    REPLAY_WORLD_DELETE_TIMEOUT_TICKS    = 20 * 30;
    static constexpr auto   DIMENSION_TRANSITION_TIMEOUT         = std::chrono::seconds{30};

    enum class CleanupState { None, WaitingForExit, ReadyToDelete, DeleteIssued };
    enum class SnapshotGamePacketPhase { StreamingChunks, WaitingAfterPlayerList, WaitingAfterEntities };
    enum class DimensionTransitionStatus { Pending, Dispatching, Succeeded, Failed, Cancelled };

    struct DimensionTransitionRequest {
        std::atomic<DimensionTransitionStatus> status{DimensionTransitionStatus::Pending};
        uint64_t                               generation{};
    };

    struct PendingSubChunkPacket {
        int                   index = -1;
        std::string           payload;
        std::vector<ChunkPos> targets;
        std::vector<ChunkPos> dependencies;
        bool                  injected{};
    };

    struct SnapshotColumnIdentity {
        int              levelChunkIndex = -1;
        std::vector<int> subChunkIndices;

        bool operator==(SnapshotColumnIdentity const&) const = default;
    };

    struct PendingSnapshotApply {
        size_t   readerIndex{};
        bool     followRecordedPlayer{};
        bool     serverPlayerRelocated{};
        uint64_t dimensionGeneration{};
    };

    int    mCurrentTick             = 0;
    size_t mReaderIndex             = 0;
    int    mChunkInjectionTicks     = 0;
    int    mChunkInjectionIdleTicks = 0;
    size_t mPendingLevelChunkCursor = 0;
    size_t mPendingSubChunkCursor   = 0;
    size_t mInjectedLevelChunks     = 0;
    size_t mInjectedSubChunkPackets = 0;
    size_t mInjectedSubChunkEntries = 0;
    size_t mReusedSnapshotColumns   = 0;
    size_t mDirectLevelChunks       = 0;
    size_t mDirectSubChunkPackets   = 0;
    size_t mDirectSubChunkEntries   = 0;

    bool                          mActive            = false;
    bool                          mIsPaused          = false;
    bool                          mReplayWorldJoined = false;
    bool                          mWorldReady        = false;
    bool                          mReplayFailed      = false;
    std::atomic<Packet const*>    mInjectingPacket{nullptr};
    std::atomic<bool>             mChunkCompletionObserved{false};
    std::atomic<Dimension const*> mReplayDimension{nullptr};
    bool                          mInitialSnapshotApplied     = false;
    bool                          mChunkInjectionPending      = false;
    bool                          mApplyingChunkSnapshot      = false;
    bool                          mChunkInjectionPlanPrepared = false;
    bool                          mCenterChunksReady          = false;
    SnapshotGamePacketPhase       mSnapshotGamePacketPhase    = SnapshotGamePacketPhase::StreamingChunks;

    std::atomic<bool>                   mStopRequested{false};
    std::atomic<int>                    mRequestedSeekTick{-1};
    int                                 mSeekTargetTick{-1};
    float                               mPlaybackSpeed{1.0f};
    float                               mPlaybackTickAccumulator{};
    std::optional<int>                  mReplayTime;
    std::optional<DimensionType>                  mPendingReplayDimension;
    std::optional<PendingSnapshotApply>           mPendingSnapshotApply;
    std::shared_ptr<DimensionTransitionRequest>   mDimensionTransitionRequest;
    int                                           mDimensionTransitionSettledUpdates = 0;
    std::atomic<uint64_t>                         mDimensionTransitionGeneration{0};
    uint64_t                                      mCompletedDimensionGeneration = 0;
    std::chrono::steady_clock::time_point         mDimensionTransitionStartedAt{};

    std::chrono::steady_clock::time_point mChunkInjectionStartedAt{};
    std::vector<double>                   mChunkInjectionDurationsMs;
    double                                mChunkPlanPreparationMs{};

    CleanupState mCleanupState              = CleanupState::None;
    int          mCleanupWaitTicks          = 0;
    bool         mOrphanReplayWorldsScanned = false;

    std::filesystem::path mReplayFilePath;
    std::string           mReplayLevelId;

    PlaybackMeta mMeta;

    std::vector<std::unique_ptr<ReplayReader>>              mReaders;
    std::vector<PlaybackSnapshotContext>                    mSnapshotContexts;
    std::vector<std::string>                                mChunkPackets;
    std::unordered_map<size_t, std::vector<int>>            mInlineLevelChunkPacketIndices;
    std::unordered_map<size_t, std::vector<int>>            mInlineSubChunkPacketIndices;
    std::mutex                                              mPendingLevelChunksMutex;
    std::unordered_multiset<ChunkPos>                       mPendingLevelChunks;
    std::unordered_set<ChunkPos>                            mCompletedLevelChunkPositions;
    std::vector<int>                                        mPendingLevelChunkIndices;
    std::unordered_set<ChunkPos>                            mSnapshotChunks;
    std::unordered_set<ChunkPos>                            mApplyingSnapshotChunks;
    std::optional<DimensionType>                            mChunkIsolationDimension;
    std::unordered_map<ChunkPos, SnapshotColumnIdentity>    mAppliedSnapshotColumns;
    std::unordered_map<ChunkPos, SnapshotColumnIdentity>    mPendingSnapshotColumns;
    std::unordered_set<ChunkPos>                            mDirtySnapshotColumns;
    std::unordered_set<ChunkPos>                            mReusableSnapshotColumns;
    std::unordered_set<ChunkPos>                            mDirectSnapshotColumns;
    std::unordered_set<int>                                 mDirectLevelChunkIndices;
    std::vector<int>                                        mPendingSubChunkIndices;
    std::vector<PendingSubChunkPacket>                      mPendingSubChunkPackets;
    std::optional<std::string>                              mPendingSnapshotLocalPlayer;
    std::vector<std::pair<MinecraftPacketIds, std::string>> mPendingSnapshotGamePackets;
    std::unordered_set<ActorUniqueID>                       mRecordedEntityIds;
    std::unordered_set<std::string>                         mReplayObjectiveNames;
    std::unordered_set<ChunkPos>                            mCenterChunkPositions;
    std::unordered_map<ChunkPos, size_t>                    mRemainingSubChunkPacketsByColumn;

    std::unordered_map<ChunkPos, std::shared_ptr<LevelChunk>> mRetainedReplayChunks;

    std::weak_ptr<MinecraftScreenModel> mScreenModel;
    Player*                             mReplayPlayer   = nullptr;
    LegacyClientNetworkHandler*         mNetworkHandler = nullptr;

public:
    bool mIsProcessingSnapshot = false;

private:
    bool init(std::filesystem::path filePath);

    void onWorldReady();

    void applyInitialSnapshot();

    void applySnapshot(ReplayReader& reader, bool followRecordedPlayer, bool serverPlayerRelocated = false);

    [[nodiscard]] bool
    ensureReplayDimension(DimensionType target, PlaybackView const& view, bool relocateWithinDimension = false);

    void processPendingDimensionTransition();

    void completeReplayDimensionTransition();

    void resetDimensionScopedReplayState();

    [[nodiscard]] bool prepareChunkInjectionPlan(PlaybackView const& view);

    [[nodiscard]] bool tryFinishChunkInjection();

    [[nodiscard]] bool finishChunkInjection();

    [[nodiscard]] bool injectPendingLevelChunks(std::chrono::steady_clock::time_point deadline);

    [[nodiscard]] bool
    injectReadySubChunkPackets(size_t& injectedPackets, std::chrono::steady_clock::time_point deadline);

    void updateCenterChunkReadiness();

    [[nodiscard]] bool injectChunkPacket(std::string_view payload, MinecraftPacketIds packetId);

    [[nodiscard]] bool applyRequestModeLevelChunkDirect(std::string_view payload);

    [[nodiscard]] bool applySubChunkDirect(std::string_view payload);

    [[nodiscard]] bool applyGamePacket(MinecraftPacketIds packetId, std::string_view payload);

    [[nodiscard]] bool applyPendingSnapshotLocalPlayer();

    void invalidateSnapshotColumns(Packet const& packet);

    [[nodiscard]] bool flushPendingSnapshotGamePackets(
        bool                                         playerListOnly,
        size_t                                       maxPackets,
        std::chrono::steady_clock::time_point const& deadline
    );

    [[nodiscard]] bool clearRecordedEntities();

    [[nodiscard]] bool refreshReplayPlayer();

    [[nodiscard]] bool clearReplayObjectives();

    void clearReplayData();

    void finishWorldCleanup();

    void beginSeek(int targetTick);

    [[nodiscard]] bool advanceReplayTick(bool stopAtEnd);

public:
    bool start(std::filesystem::path filePath);
    void stop();

    void requestStop() { mStopRequested.store(true, std::memory_order_release); }

    void requestSeek(int tick) { mRequestedSeekTick.store(tick < 0 ? 0 : tick, std::memory_order_release); }

    void tick();

    // Advances dimension transitions while the loading screen pauses the native level tick.
    void updateControlPlane();

    [[nodiscard]] bool isActive() const { return mActive; }

    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    [[nodiscard]] bool hasJoinedReplayWorld() const { return mReplayWorldJoined; }

    [[nodiscard]] int getCurrentTick() const { return mCurrentTick; }

    [[nodiscard]] int getTotalTicks() const;

    [[nodiscard]] float getPlaybackSpeed() const { return mPlaybackSpeed; }

    void adjustPlaybackSpeed(int direction);

    [[nodiscard]] bool setPaused(bool paused);

    [[nodiscard]] bool isInjectingPacket(Packet const* packet) const {
        return packet && mInjectingPacket.load(std::memory_order_acquire) == packet;
    }

    [[nodiscard]] bool isIsolatingReplayWorld() const { return mActive; }

    [[nodiscard]] bool shouldIsolateChunkPackets() const;

    [[nodiscard]] bool shouldSuppressNativeChunk(ChunkPos const& pos, DimensionType packetDimension) const;

    [[nodiscard]] bool isReplayWorldCleanupPending() const { return mCleanupState != CleanupState::None; }

    [[nodiscard]] static bool isReplayLevel(Level const& level);

    void setMinecraftScreenModel(std::shared_ptr<MinecraftScreenModel> const& screenModel);

    void onLevelJoined(Player& player);

    void onLevelStartJoin();

    void onLevelExit();

    void onLevelJoinCancelled();

    void tryFinalizeWorldCleanup();

    void captureNetworkContext(LegacyClientNetworkHandler& handler);

    void clearNetworkContext();

    void onLevelChunkHandled(ChunkPos const& pos, Dimension const& dimension);

    void handleNextTick();

    void handleSnapshotContext(PlaybackSnapshotContext const& context);

    void handleCreateLocalPlayer(PlaybackBuffer& data);

    bool sendRecordedTickPacket();

    void handleLevelChunkCached(int index);

    void handleSubChunkCached(int index);

    [[nodiscard]] int cacheInlineChunkPacket(MinecraftPacketIds packetId, std::string payload);

    void handleGamePacket(PlaybackBuffer& data);

    void handleMoveEntities(PlaybackBuffer& data);

private:
    ReplaySession() = default;
    ~ReplaySession();

public:
    [[nodiscard]] static ReplaySession& getInstance() {
        static ReplaySession instance;
        return instance;
    }
};

} // namespace playback::functions
