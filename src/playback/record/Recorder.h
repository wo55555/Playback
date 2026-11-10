#pragma once

#include "playback/io/AsyncReplaySaver.h"
#include "playback/io/ReplayResourcePacks.h"
#include "playback/packet/PlaybackSetEquipmentPacket.h"
#include "playback/utils/container/LinkedHashMap.h"
#include "playback/visuals/ReplayThumbnail.h"

#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/platform/UUID.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/dimension/DimensionType.h"

#include "ll/api/thread/ThreadPoolExecutor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Block;
class Player;
class Level;
class LevelChunk;
class LevelChunkPacket;
class SubChunkPacket;
class Packet;
class Dimension;
class SaveContext;

namespace playback::packet {
struct PacketLifecycleSemantics;
}

namespace playback::record {

// IDs every recording stores the local player under.
inline constexpr uint64_t RecordedPlayerRuntimeIdValue = uint64_t{1} << 62;
inline constexpr int64_t  RecordedPlayerUniqueIdValue  = std::numeric_limits<int64_t>::max() - 1024;

using playback::io::AsyncReplaySaver;
using playback::io::PlaybackBuffer;
using playback::io::PlaybackSerializedGamePacket;
using playback::io::PlaybackSnapshotContext;
using playback::packet::PacketLifecycleSemantics;
using playback::packet::PlaybackSetEquipmentPacket;

struct PlaybackView {
    float x     = 0.0f;
    float y     = 0.0f;
    float z     = 0.0f;
    float yaw   = 0.0f;
    float pitch = 0.0f;
};

// Camera position minus entity position, and the rendered FOV in degrees.
struct PlaybackCameraSample {
    float x   = 0.0f;
    float y   = 0.0f;
    float z   = 0.0f;
    float fov = 0.0f;
    // Absolute game camera pose, only captured while the vanilla camera is not bound to the entity (sleeping).
    bool  hasView   = false;
    float viewX     = 0.0f;
    float viewY     = 0.0f;
    float viewZ     = 0.0f;
    float viewYaw   = 0.0f;
    float viewPitch = 0.0f;
};

struct PlaybackChunkMeta {
    int  duration          = 0;
    bool forcePlaySnapshot = false;
};

struct PlaybackMeta {
    std::string name = "Unnamed";
    std::string worldName;
    int         totalTicks = 0;
    // Packet layouts differ per game version; empty means the file predates this field.
    std::string gameVersion;

    utils::container::LinkedHashMap<std::string, PlaybackChunkMeta> chunks;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;

    [[nodiscard]] static std::string currentGameVersion();
    [[nodiscard]] bool               isCompatibleWithRuntime() const;
};

class Recorder {
private:
    enum class State { Idle, Recording, Paused, Closing };
    enum class SnapshotCaptureResult { Success, NotReady, Failed };
    // Local player view after one rendered frame; fraction is the frame's partial tick after the previous tick.
    struct LocalViewFrame {
        float fraction{};
        float pitch{};
        float yaw{};
    };
    struct SnapshotDimension {
        DimensionType id{};
        int32_t       minHeight{};
        int32_t       maxHeight{};
        std::string   name;
    };
    // A portable column whose serialization waits for the tick flush, where it can run off the coroutine and in
    // parallel.
    struct DeferredCompletedChunk {
        ChunkPos                    pos;
        DimensionType               dimensionId{};
        int                         minimumSubChunkY{};
        std::shared_ptr<LevelChunk> chunk;
    };
    // Marks the timeline slot of a DeferredCompletedChunk inside mPendingGamePackets.
    static constexpr int32_t DeferredChunkPacketId = -1;
    // A column's packets from the previous snapshot, reused while the same column object shows no change.
    struct SnapshotColumnCacheEntry {
        std::weak_ptr<LevelChunk>         chunk;
        uint64_t                          subChunkFingerprint{};
        uint64_t                          chunkFingerprint{};
        std::shared_ptr<LevelChunkPacket> levelChunk;
        std::shared_ptr<SubChunkPacket>   subChunk;
    };
    struct SnapshotColumn {
        ChunkPos                    pos;
        std::shared_ptr<LevelChunk> chunk;
        // A copy, so a background job never reads the live cache.
        std::optional<SnapshotColumnCacheEntry> cached;
        bool                                    packetDirty{};
    };
    struct SnapshotColumnResult {
        std::shared_ptr<LevelChunkPacket> levelChunk;
        std::shared_ptr<SubChunkPacket>   subChunk;
        // Read before serializing, so a change that lands during serialization shows up as a mismatch.
        uint64_t    subChunkFingerprint{};
        uint64_t    chunkFingerprint{};
        std::string error;
        bool        done{};
    };
    // The terrain half of a snapshot. Columns and results are index-aligned; workers claim columns through next.
    struct SnapshotColumnsJob {
        Player*                           player{};
        PlaybackView                      view;
        PlaybackCameraSample              camera;
        DimensionType                     dimension{};
        int                               minHeight{};
        int                               maxHeight{};
        std::string                       dimensionName;
        Block const*                      air{};
        std::vector<SnapshotColumn>       columns;
        std::vector<SnapshotColumnResult> results;
        // Dirty marks this capture consumed; they go back to the pending set unless the snapshot commits.
        std::unordered_set<ChunkPos> dirtyTaken;
        // A sliced job records a column error and moves on; its final pass decides what the error means.
        bool                  stopOnError{true};
        bool                  finalPass{};
        bool                  initial{};
        std::atomic<size_t>   next{0};
        std::atomic<uint64_t> sliceDeadlineNs{0}; // 0 means no deadline
        // Sliced jobs only: pool threads that join the client thread in each slice.
        size_t   sliceHelpers{};
        int      slices{};
        uint64_t sliceTotalNs{};
        uint64_t sliceMaxNs{};
        uint64_t launchCostNs{};
        // Copied off the background job when the final pass replaces it. The final pass does not inherit the
        // background dirty set: a matching fingerprint already has those bytes, and marks that have not landed yet
        // stay carried for the next snapshot.
        int                                 backgroundSlices{};
        uint64_t                            backgroundSliceTotalNs{};
        uint64_t                            backgroundSliceMaxNs{};
        uint64_t                            backgroundLaunchCostNs{};
        size_t                              backgroundColumns{};
        std::unordered_set<ChunkPos>        backgroundDirty;
        std::chrono::steady_clock::duration barrierWait{};

        SnapshotColumnsJob()                                     = default;
        SnapshotColumnsJob(SnapshotColumnsJob const&)            = delete;
        SnapshotColumnsJob& operator=(SnapshotColumnsJob const&) = delete;
    };

    std::unique_ptr<AsyncReplaySaver>                     mAsyncReplaySaver;
    std::unique_ptr<playback::io::packs::PackEmbedWorker> mPackWorker;

    std::vector<std::shared_ptr<LevelChunkPacket>> mSnapshotLevelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>   mSnapshotSubChunks;
    std::vector<PlaybackSerializedGamePacket>      mSnapshotConfigurationPackets;
    std::vector<PlaybackSerializedGamePacket>      mSnapshotEntityPackets;
    std::optional<std::string>                     mSnapshotLocalPlayerPayload;
    // Client thread only. Replaced only by a successful capture; a failed one returns its dirty marks.
    std::unordered_map<ChunkPos, SnapshotColumnCacheEntry> mSnapshotColumnCache;
    std::optional<DimensionType>                           mSnapshotColumnCacheDimension;
    // Columns touched by terrain packets since the last capture; taken under the chunk mutation barrier.
    std::unordered_set<ChunkPos> mSnapshotDirtyColumns;
    std::mutex                   mSnapshotDirtyColumnsMutex;
    // A packet can be marked before its change lands, so dirty columns stay dirty for one more capture.
    std::unordered_set<ChunkPos> mSnapshotCarriedDirtyColumns;
    // Chunk rotation terrain serialized in the background while ticks keep going into the current chunk.
    std::unique_ptr<SnapshotColumnsJob>       mRotationJob;
    std::vector<PlaybackSerializedGamePacket> mConfigurationPackets;
    std::unordered_map<int32_t, size_t>       mConfigurationPacketIndices;
    // Original login packet wins. A synthesized registry is dropped when the next recording starts.
    enum class ItemRegistrySource : uint8_t { None, Original, Synthetic };
    ItemRegistrySource                        mItemRegistrySource{ItemRegistrySource::None};
    int                                       mItemRegistryItems{};
    int                                       mItemRegistryCustom{};
    int                                       mItemRegistryComponentBased{};
    bool                                      mRecordingSyntheticItemRegistry{false};
    std::vector<PlaybackSerializedGamePacket> mPendingGamePackets;
    // One entry per DeferredChunkPacketId slot in mPendingGamePackets, in the same order.
    std::vector<DeferredCompletedChunk>                       mDeferredCompletedChunks;
    mutable std::mutex                                        mPendingGamePacketsMutex;
    std::unordered_map<int32_t, std::unordered_set<ChunkPos>> mPendingPortableChunks;
    std::mutex                                                mPendingPortableChunksMutex;
    std::unordered_map<int32_t, uint64_t>                     mRecordedGamePacketCounts;
    // Position, pitch, yaw, head yaw, body yaw, in the order ActionMoveEntities writes them.
    struct EntityMovementState {
        std::array<float, 7> values{};
        bool                 onGround{};

        // Bitwise, so NaN and signed zero behave like the serialized bytes they replace.
        [[nodiscard]] bool sameAs(EntityMovementState const& other) const {
            return onGround == other.onGround && std::memcmp(values.data(), other.values.data(), sizeof(values)) == 0;
        }
    };
    std::unordered_map<ActorUniqueID, EntityMovementState> mLastEntityMovements;
    std::unordered_map<ActorUniqueID, EntityMovementState> mCurrentEntityMovements;
    std::optional<ActorUniqueID>                           mRecordedLocalPlayerId;
    std::optional<ActorRuntimeID>                          mRecordedLocalPlayerRuntimeId;
    std::optional<mce::UUID>                               mRecordedLocalPlayerUuid;
    std::optional<std::string>                             mLastLocalPlayerDataPacket;
    PlaybackBuffer                                         mLocalPlayerDataBuffer;
    std::optional<PlaybackSetEquipmentPacket>              mLastLocalPlayerEquipmentPacket;
    std::optional<int>                                     mLastLocalPlayerSwingTime;
    // Inventory slots 0-35, armor, offhand, attributes and the selected slot. Must stay >= Recorder.cpp HudCacheSize.
    std::array<std::optional<std::string>, 48> mLastLocalHudPackets;
    // Last item written for inventory slots 0-35, armor and the offhand. Unchanged slots skip serialization.
    std::array<ItemStack, 41>  mLastLocalHudItems{};
    int                        mLastLocalSelectedSlot{-1};
    std::array<float, 18>      mLastLocalAttributeValues{};
    bool                       mLastLocalAttributeValuesValid{};
    int                        mLastLocalGameTypeValue{-1};
    std::optional<std::string> mLastLocalGameTypePacket;
    // Map id whose full data was last written for the main hand and the offhand.
    std::array<std::optional<int64_t>, 2> mLastLocalHeldMapIds;
    std::optional<PlaybackView>           mSnapshotView;
    std::optional<PlaybackCameraSample>   mSnapshotCamera;
    std::optional<SnapshotDimension>      mSnapshotDimension;

    // Frames rendered since the last tick; the view turns per frame while the rest of the pose moves per tick.
    std::mutex                    mLocalViewMutex;
    std::vector<LocalViewFrame>   mLocalViewFrames;
    bool                          mLocalViewGap{true}; // frames were missed since the last tick
    std::optional<LocalViewFrame> mLocalViewPrevious;  // view at the last tick
    uint64_t                      mLocalViewTicks{};   // ticks that wrote samples
    uint64_t                      mLocalViewSamples{};
    int                           mLocalViewRate{20}; // samples per second, fixed when the recording starts

    std::optional<DimensionType>        mRecordingDimension;
    std::string                         mSnapshotFailure;
    std::chrono::steady_clock::duration mLongestSnapshotStall{};

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic<State>                                    mState{State::Idle};
    std::atomic<visuals::ReplayThumbnailCaptureProvider*> mThumbnailCaptureProvider{};
    std::atomic_bool                                      mNeedsInitialSnapshot       = true;
    std::atomic_bool                                      mDimensionTransitionPending = false;
    std::atomic<int>                                      mDimensionTransitionTargetId{0};

    int mChunkIndex           = 0;
    int mTicksInCurrentChunk  = 0;
    int mWrittenTicks         = 0;
    int mThumbnailRequestTick = 0;

    bool mHasOpenChunk                  = false;
    bool mOpenChunkHasData              = false;
    bool mCurrentChunkForcePlaySnapshot = false;
    bool mThumbnailCaptureRequested     = false;
    bool mPendingDimensionSnapshot      = false;

    std::filesystem::path mPendingRecordPath;
    std::string           mPendingRecordingName;
    std::atomic_bool      mHasPendingRecording = false;

    // Snapshot and chunk flush workers; created on first use, client thread only.
    std::unique_ptr<ll::thread::ThreadPoolExecutor> mWorkerPool;

    // Zips the finished recording off the client thread; embedded resource packs make this take tens of seconds.
    // Declared last so it is joined before the rest of the recorder is destroyed.
    std::jthread mExportThread;

private:
    static constexpr int RECORD_CHUNK_TICKS      = 20 * 60 * 5;
    static constexpr int THUMBNAIL_CAPTURE_TICKS = 20;
    // The armed capture takes the next presented frame; this leaves room for slow frames.
    static constexpr int THUMBNAIL_SETTLE_TICKS = 10;
    // A rotation job still unfinished after this many slices finishes its remaining columns in one slice.
    static constexpr int ROTATION_JOB_MAX_SLICES = 100;

    [[nodiscard]] SnapshotCaptureResult
    captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait, std::chrono::milliseconds barrierTimeout);

    // Walks the loaded columns under the mutation barrier. Cached entries are copied when their dimension matches.
    [[nodiscard]] SnapshotCaptureResult prepareSnapshotColumns(
        SnapshotColumnsJob&                                           job,
        std::chrono::steady_clock::duration&                          barrierWait,
        std::chrono::milliseconds                                     barrierTimeout,
        std::unordered_map<ChunkPos, SnapshotColumnCacheEntry> const* cache,
        std::optional<DimensionType>                                  cacheDimension,
        std::unordered_set<ChunkPos> const*                           carriedDirty
    );

    static void serializeSnapshotColumns(SnapshotColumnsJob& job);

    // Runs work on up to `helpers` pool threads and the calling thread, and returns once all of them finish.
    void runParallel(size_t helpers, std::function<void()> const& work);

    // Serializes the unfinished columns on worker threads and the client thread, and waits for them.
    void runSnapshotColumns(SnapshotColumnsJob& job);

    [[nodiscard]] SnapshotCaptureResult finishSnapshotColumns(SnapshotColumnsJob& job);

    [[nodiscard]] SnapshotCaptureResult launchRotationSnapshot(std::chrono::milliseconds barrierTimeout);

    // One bounded slice of the rotation job; true once every column has been claimed.
    [[nodiscard]] bool runRotationSlice();

    // Replaces the background job with a sliced final pass. On failure the background job is put back.
    [[nodiscard]] SnapshotCaptureResult beginSnapshotFinalPass();

    [[nodiscard]] SnapshotCaptureResult finishRotationSnapshot(std::chrono::steady_clock::duration& barrierWait);

    void discardRotationSnapshot();

    void restoreSnapshotDirtyColumns(SnapshotColumnsJob& job);

    [[nodiscard]] bool commitChunkSnapshot(
        std::chrono::steady_clock::duration captureElapsed,
        std::chrono::steady_clock::duration barrierWait
    );

    [[nodiscard]] bool writeInitialSnapshotIfNeeded();

    [[nodiscard]] bool writeCreateLocalPlayer();

    [[nodiscard]] bool writeSnapshot();

    [[nodiscard]] bool writeTickBoundary();

    [[nodiscard]] bool flushGamePackets();

    [[nodiscard]] bool writeEntityMovements();
    [[nodiscard]] bool writeLocalCameraOffset();
    [[nodiscard]] bool writeLocalViewSamples();
    // Ticks passed without writing the view, so the buffered frames no longer belong to the next tick.
    void discardLocalViewFrames();

    [[nodiscard]] bool writeLocalPlayerState();

    [[nodiscard]] bool finishCurrentChunk(bool close);

    void failRecording(std::string_view reason);

    void cancelRecording(std::string_view reason);

    void abortRecording();

    // Ends the recording up to a finished temp folder; false when nothing usable was recorded.
    [[nodiscard]] bool closeRecording();

    [[nodiscard]] std::filesystem::path finishSaver();

    [[nodiscard]] bool exportRecording(
        std::filesystem::path const& replayPath,
        std::string const&           fileBaseName,
        std::string_view             metaName
    );

    void clearPendingRecording();

    void logRecordedGamePacketSummary() const;

    void resetStateForNewRecording();

    void resetChunkSnapshot();

    void resetSnapshotColumnCache();

    void markSnapshotColumnDirty(ChunkPos const& pos);

    void markSnapshotColumnsDirty(Packet const& packet);

    void recordConfigurationPacket(
        Packet const&                   packet,
        PacketLifecycleSemantics const& semantics,
        std::string_view                payload = {}
    );

    void ensureRecordedItemRegistry(::Level& level);

    void recordSpawnedActor(ActorRuntimeID runtimeId, Packet const& fallbackPacket);

    void recordLevelChunkPacket(LevelChunkPacket const& packet, std::string_view payload);

    void resolveDeferredCompletedChunks(
        std::vector<PlaybackSerializedGamePacket>& pending,
        std::vector<DeferredCompletedChunk>&       deferred
    );

    [[nodiscard]] static bool serializeDeferredCompletedChunk(
        DeferredCompletedChunk const& job,
        Block const&                  air,
        SaveContext const&            saveContext,
        std::string&                  levelChunkPayload,
        std::string&                  subChunkPayload
    );

public:
    Recorder();

    [[nodiscard]] bool isActive() const {
        auto const state = mState.load();
        return state == State::Recording || state == State::Paused;
    }

    [[nodiscard]] bool isPaused() const { return mState.load() == State::Paused; }

    // Client thread only; true until the recording thumbnail frame has been taken, so overlays can stay out of it.
    [[nodiscard]] bool isThumbnailPending() const {
        if (mState.load() != State::Recording || !mThumbnailCaptureProvider.load(std::memory_order_acquire)) {
            return false;
        }
        return !mThumbnailCaptureRequested || mWrittenTicks < mThumbnailRequestTick + THUMBNAIL_SETTLE_TICKS;
    }
    // Client thread only; stops advancing while paused.
    [[nodiscard]] int recordedTicks() const { return mWrittenTicks; }

    void start();
    void pause();
    // Saves directly under a timestamp name, including a recording still waiting for a name.
    void stop();
    // Stops and deletes everything recorded so far.
    void discard();

    // Stops but keeps the data until savePendingRecording or discardPendingRecording.
    [[nodiscard]] bool finishToPending();
    [[nodiscard]] bool hasPendingRecording() const { return mHasPendingRecording.load(std::memory_order_acquire); }
    // Client thread only.
    [[nodiscard]] std::string const& pendingRecordingName() const { return mPendingRecordingName; }
    // An empty name keeps the timestamp name; false keeps the data pending.
    [[nodiscard]] bool savePendingRecording(std::string_view name);
    void               discardPendingRecording();

    // Blocks until the last stopped recording has been written to its replay file.
    void waitForExport();

    // Joins the worker threads before the mod unloads; the next snapshot creates them again.
    void releaseWorkerPool();

    void setThumbnailCaptureProvider(visuals::ReplayThumbnailCaptureProvider* provider) {
        mThumbnailCaptureProvider.store(provider, std::memory_order_release);
    }

    // Every packet the client handles, around its handler.
    void recordReceivedPacket(Packet const& packet, std::string_view payload);

    void recordHandledPacket(Packet const& packet);

    // A payload is the packet as received; it is kept instead of writing the decoded packet back out.
    void recordGamePacket(Packet const& packet, std::string_view payload = {});

    void recordCompletedChunk(ChunkPos const& pos, Dimension const& dimension);

    // Called after every rendered frame with the frame's partial tick.
    void recordLocalViewFrame(float alpha);

    void endTick(bool close);

public:
    [[nodiscard]] static Recorder& getInstance() {
        static Recorder instance;
        return instance;
    }
};

class ReplayExporter {
private:
    static std::optional<PlaybackMeta> tryReadMeta(std::filesystem::path const& file);

public:
    static bool exportReplay(
        std::filesystem::path const& recordDir,
        std::filesystem::path const& outputFile,
        std::string_view             name
    );
};

[[nodiscard]] bool hookNetwork(bool);

// Last FOV (degrees) the renderer used for the player's view while recording hooks were installed; 0 before the first
// frame.
[[nodiscard]] float latestRenderedFov() noexcept;

} // namespace playback::record
