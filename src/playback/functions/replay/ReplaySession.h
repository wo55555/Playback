#pragma once

#include "playback/functions/record/Recorder.h"

#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <chrono>
#include <cstddef>
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
class LegacyClientNetworkHandler;
class MinecraftScreenModel;
class Player;
enum class MinecraftPacketIds : int;

namespace playback::functions {

class ReplaySession {
private:
    static constexpr size_t LEVEL_CHUNK_PACKETS_PER_TICK        = 16;
    static constexpr size_t MAX_LEVEL_CHUNKS_IN_FLIGHT          = 32;
    static constexpr size_t SUB_CHUNK_PACKETS_PER_TICK          = 16;
    static constexpr size_t SUB_CHUNK_ENTRIES_PER_TICK          = 384;
    static constexpr int    CHUNK_INJECTION_STALL_TIMEOUT_TICKS = 20 * 30;
    static constexpr int    REPLAY_WORLD_DELETE_TIMEOUT_TICKS   = 20 * 30;

    enum class CleanupState { None, WaitingForExit, ReadyToDelete, DeleteIssued };

    struct PendingSubChunkPacket {
        int                   index = -1;
        size_t                entryCount{};
        std::vector<ChunkPos> targets;
        std::vector<ChunkPos> dependencies;
        bool                  injected{};
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

    std::chrono::steady_clock::time_point mChunkInjectionStartedAt{};
    std::vector<double>                   mChunkInjectionDurationsMs;
    double                                mChunkPlanPreparationMs{};

    CleanupState mCleanupState     = CleanupState::None;
    int          mCleanupWaitTicks = 0;

    std::filesystem::path mReplayFilePath;
    std::string           mReplayLevelId;

    PlaybackMeta mMeta;

    std::vector<std::unique_ptr<ReplayReader>> mReaders;
    std::vector<std::optional<PlaybackView>>   mSnapshotViews;
    std::vector<std::string>                   mChunkPackets;
    std::mutex                                 mPendingLevelChunksMutex;
    std::unordered_multiset<ChunkPos>          mPendingLevelChunks;
    std::unordered_set<ChunkPos>               mCompletedLevelChunkPositions;
    std::vector<int>                           mPendingLevelChunkIndices;
    std::unordered_set<ChunkPos>               mSnapshotChunks;
    std::unordered_set<ChunkPos>               mApplyingSnapshotChunks;
    std::vector<int>                           mPendingSubChunkIndices;
    std::vector<PendingSubChunkPacket>         mPendingSubChunkPackets;
    std::unordered_set<ChunkPos>               mCenterChunkPositions;
    std::unordered_map<ChunkPos, size_t>       mRemainingSubChunkPacketsByColumn;

    std::weak_ptr<MinecraftScreenModel> mScreenModel;
    Player*                             mReplayPlayer   = nullptr;
    LegacyClientNetworkHandler*         mNetworkHandler = nullptr;

public:
    bool mIsProcessingSnapshot = false;

private:
    bool init(std::filesystem::path filePath);

    void onWorldReady();

    void applyInitialSnapshot();

    void applySnapshot(ReplayReader& reader);

    [[nodiscard]] bool prepareChunkInjectionPlan(PlaybackView const& view);

    [[nodiscard]] bool tryFinishChunkInjection();

    [[nodiscard]] bool finishChunkInjection();

    [[nodiscard]] bool injectPendingLevelChunks(std::chrono::steady_clock::time_point deadline);

    [[nodiscard]] bool injectReadySubChunkPackets(
        size_t&                               injectedPackets,
        size_t&                               injectedEntries,
        std::chrono::steady_clock::time_point deadline
    );

    void updateCenterChunkReadiness();

    [[nodiscard]] std::optional<PlaybackView> deriveLegacySnapshotView() const;

    [[nodiscard]] bool injectChunkPacket(std::string_view payload, MinecraftPacketIds packetId);

    void clearReplayData();

    void finishWorldCleanup();

public:
    bool start(std::filesystem::path filePath);
    void stop();

    void tick();

    [[nodiscard]] bool isActive() const { return mActive; }

    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    [[nodiscard]] bool setPaused(bool paused);

    [[nodiscard]] bool isInjectingPacket(Packet const* packet) const {
        return packet && mInjectingPacket.load(std::memory_order_acquire) == packet;
    }

    [[nodiscard]] bool isIsolatingReplayWorld() const { return mActive; }

    [[nodiscard]] bool shouldIsolateChunkPackets() const;

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

    bool sendRecordedTickPacket();

    void handleLevelChunkCached(int index);

    void handleSubChunkCached(int index);

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
