#pragma once

#include "playback/keyframe/CameraRenderState.h"
#include "playback/record/Recorder.h"
#include "playback/state/editing/models/MathTypes.h"
#include "playback/state/editing/models/Track.h"
#include "playback/visuals/ReplayEntityInterpolator.h"
#include "playback/visuals/ReplayWorldEnvHooks.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
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
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Level;
class Dimension;
class LevelChunk;
class LevelChunkPacket;
class Actor;
class LegacyClientNetworkHandler;
class MinecraftScreenModel;
class Player;
class SerializedSkinRef;
class CompoundTag;
class ItemRegistryPacket;
class ResourcePacksInfoPacket;
class ResourcePackStackPacket;
class StartGamePacket;
struct DimensionArguments;
struct FollowedHudMirror;
enum class MinecraftPacketIds : int;

namespace playback::replay {
using playback::io::AsyncReplaySaver;
using playback::io::PlaybackBuffer;
using playback::io::PlaybackSerializedGamePacket;
using playback::io::PlaybackSnapshotContext;
using playback::io::ReplayReader;
using playback::io::ReplayWriter;
using playback::record::PlaybackChunkMeta;
using playback::record::PlaybackMeta;
using playback::record::PlaybackView;

using SteadyTimePoint = std::chrono::steady_clock::time_point;

enum class ReplayExportTickState : uint8_t { Unavailable, Waiting, Ready, Invalid, Failed };

enum class ReplayExportTimelinePhase : uint8_t { Inactive, Initializing, Continuous };

// None: no camera track under the playhead. Keyframe: a keyframe track drives the camera. Person: a first- or
// third-person track follows its subject.
enum class PreviewShotMode : uint8_t { None, Keyframe, Person };

struct ReplayCameraViewpoint {
    float x{};
    float y{};
    float z{};
    float pitch{};
    float yaw{};
    float roll{};
    float fov{70.0f};
};

class ReplaySession {
private:
    static constexpr size_t MAX_LEVEL_CHUNKS_IN_FLIGHT = 64;
    // A seek waits on its batch, so it keeps more columns loading at once than normal playback.
    static constexpr size_t SEEK_LEVEL_CHUNKS_IN_FLIGHT         = 128;
    static constexpr size_t MAX_SUB_CHUNK_ENTRIES_PER_PACKET    = 1536;
    static constexpr size_t SNAPSHOT_GAME_PACKETS_PER_TICK      = 16;
    static constexpr int    CHUNK_INJECTION_STALL_TIMEOUT_TICKS = 20 * 30;
    static constexpr int    DIMENSION_TRANSITION_SETTLE_UPDATES = 2;
    static constexpr int    REPLAY_WORLD_DELETE_TIMEOUT_TICKS   = 20 * 30;
    static constexpr auto   DIMENSION_ACK_FALLBACK_DELAY        = std::chrono::seconds{1};
    static constexpr auto   DIMENSION_TRANSITION_TIMEOUT        = std::chrono::seconds{30};

    enum class CleanupState { None, WaitingForExit, ReadyToDelete, DeleteIssued };
    enum class SnapshotGamePacketPhase { StreamingChunks, WaitingAfterPlayerList, WaitingAfterEntities };
    enum class DimensionTransitionStatus { Pending, Dispatching, Succeeded, Failed, Cancelled };

    struct DimensionTransitionRequest {
        std::atomic<DimensionTransitionStatus> status{DimensionTransitionStatus::Pending};
        std::atomic<bool>                      acknowledgmentFallbackQueued{false};
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

    struct RecordedDimensionHeightRange {
        int32_t minimum{};
        int32_t maximum{};

        bool operator==(RecordedDimensionHeightRange const&) const = default;
    };

    struct ReplayDimensionProfile {
        std::string                                           levelId;
        std::unordered_map<int, RecordedDimensionHeightRange> heightRanges;
        std::unordered_map<int, std::string>                  names;
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

    // A seek holds timeline chunk packets back until its landing is near, so catch-up streams one merged batch.
    // Block updates for those columns wait too, or the older chunk data would overwrite them when it lands.
    // A packet the plan would skip anyway (blob cache, other dimension) passes through untouched.
    struct SeekDeferredLevelChunk {
        int      index{};
        uint64_t sequence{};
        ChunkPos pos;
        bool     passthrough{};
    };
    struct SeekDeferredSubChunkEntry {
        ChunkPos column;
        int      subChunk{};
        bool     successful{};
    };
    struct SeekDeferredSubChunk {
        int                                    index{};
        uint64_t                               sequence{};
        bool                                   passthrough{};
        std::vector<SeekDeferredSubChunkEntry> entries;
    };
    struct SeekHeldBlockPacket {
        MinecraftPacketIds id{};
        std::string        payload;
        ChunkPos           column;
        int                subChunk{};
        uint64_t           sequence{};
    };
    bool                                   mSeekChunksDeferred{};
    uint64_t                               mSeekDeferredSequence{};
    std::vector<SeekDeferredLevelChunk>    mSeekDeferredLevelChunkPackets;
    std::vector<SeekDeferredSubChunk>      mSeekDeferredSubChunkPackets;
    std::unordered_map<ChunkPos, uint64_t> mSeekLatestLevelChunks;
    // A request-mode LevelChunk for a loaded column keeps its sub chunks, so only one with block data replaces them.
    std::unordered_map<ChunkPos, uint64_t>                          mSeekLatestBlockLevelChunks;
    std::unordered_map<ChunkPos, std::unordered_map<int, uint64_t>> mSeekLatestSubChunks;
    // Set until the pending injection batch that includes a seek flush completes.
    bool                             mSeekFlushedBatch{};
    std::vector<SeekHeldBlockPacket> mSeekHeldBlockPackets;
    struct SeekReleasedPacket {
        MinecraftPacketIds id{};
        std::string        payload;
        bool               recordedLocalPlayer{};
    };
    // Packets held for a flushed batch, applied in order once it lands.
    std::vector<SeekReleasedPacket> mSeekReleasedPackets;
    // An entity spawning into a held column that the client has not loaded stops the tick until the batch lands.
    bool mSeekTickInterrupted{};
    bool mSeekWindowEntered{};
    bool mSeekTickResuming{};

    std::atomic<bool>                    mStopRequested{false};
    std::atomic<int>                     mRequestedSeekTick{-1};
    int                                  mSeekTargetTick{-1};
    bool                                 mExportSeekRequested{};
    bool                                 mSnapMovementDuringSeek{};
    bool                                 mSeekParticlesCleared{};
    bool                                 mSeekParticlesStale{};
    ReplayExportTimelinePhase            mExportTimelinePhase{ReplayExportTimelinePhase::Inactive};
    int                                  mExportTargetTick{-1};
    bool                                 mExportForceSnapshotOnSeek{false};
    std::optional<ReplayCameraViewpoint> mExportCameraViewpoint;
    float                                mPlaybackSpeed{1.0f};
    float                                mClipSpeedScale{1.0f};
    float                                mPlaybackTickAccumulator{};
    std::atomic<SteadyTimePoint>         mTickAdvancedAt{};
    std::atomic<float>                   mFrozenPreviewPartial{-1.0f};
    mutable std::mutex                   mPreviewFreezeMutex;
    state::editing::model::FreezeTrack   mPreviewFreezeTrack;
    int                                  mPreviewPlayheadTick{0};
    bool                                 mResumeAfterFreezeSeek{false};

    bool                                        mObserverPreviewInRange{false};
    std::optional<keyframe::CameraRenderState>  mPausedPreviewPose;
    uint64_t                                    mPausedPreviewSeekCount{};
    std::atomic<bool>                           mFollowRecordedPlayer{false};
    std::atomic<bool>                           mFollowThirdPerson{false};
    // The camera track the preview is on: a keyframe track suspends the user's follow, a person track follows its
    // subject in its place.
    std::atomic<PreviewShotMode>                mPreviewShotMode{PreviewShotMode::None};
    std::atomic<bool>                           mShotThirdPerson{false};
    bool                                        mFollowViewApplied{false};
    bool                                        mFollowViewThirdApplied{false};
    bool                                        mTeleportToSubjectPending{false};
    std::optional<keyframe::CameraRenderState>  mPendingObserverPark;
    std::optional<keyframe::CameraRenderState>  mFollowHeldPose;
    bool                                        mFollowExitParkPending{false};
    std::atomic<uint64_t>                       mSeekFinishCount{0};
    uint64_t                                    mObserverPreviewGeneration{};
    ::Vec3                                      mLastObserverPreviewFeet{};
    ::Vec2                                      mLastObserverPreviewRotation{};
    std::optional<ChunkPos>                     mLastObserverServerSyncChunk;
    bool                                        mObserverServerPositionDirty{};
    std::atomic<uint64_t>                       mObserverServerSyncEpoch{0};
    std::atomic<uint64_t>                       mObserverServerSyncSequence{0};
    std::optional<int>                          mReplayTime;
    bool                                        mRecordedDoDaylightCycle{true};
    bool                                        mRecordedDoWeatherCycle{true};
    float                                       mRecordedRainLevel{0.0f};
    float                                       mRecordedLightningLevel{0.0f};
    std::optional<double>                       mTimeOfDayOverride;
    visuals::ReplayWeatherMode                  mWeatherOverride{visuals::ReplayWeatherMode::Recorded};
    visuals::ReplayWeatherMode                  mLastAppliedWeatherMode{visuals::ReplayWeatherMode::Recorded};
    bool                                        mWeatherRendererNeedsPrime{false};
    std::optional<::Vec3>                       mLastPrimedWeatherPos;
    std::optional<DimensionType>                mPendingReplayDimension;
    std::optional<PendingSnapshotApply>         mPendingSnapshotApply;
    std::shared_ptr<DimensionTransitionRequest> mDimensionTransitionRequest;
    int                                         mDimensionTransitionSettledUpdates = 0;
    std::atomic<uint64_t>                       mDimensionTransitionGeneration{0};
    uint64_t                                    mCompletedDimensionGeneration = 0;
    std::chrono::steady_clock::time_point       mDimensionTransitionStartedAt{};

    std::chrono::steady_clock::time_point mChunkInjectionStartedAt{};
    std::vector<double>                   mChunkInjectionDurationsMs;
    double                                mChunkPlanPreparationMs{};

    CleanupState mCleanupState              = CleanupState::None;
    int          mCleanupWaitTicks          = 0;
    bool         mOrphanReplayWorldsScanned = false;

    std::filesystem::path                                      mReplayFilePath;
    std::string                                                mReplayLevelId;
    std::atomic<std::shared_ptr<ReplayDimensionProfile const>> mReplayDimensionProfile;

    PlaybackMeta mMeta;

    std::vector<std::unique_ptr<ReplayReader>>                  mReaders;
    std::vector<PlaybackSnapshotContext>                        mSnapshotContexts;
    std::vector<int>                                            mDimensionTransitionTicks;
    std::vector<std::string>                                    mChunkPackets;
    std::unordered_map<size_t, std::vector<int>>                mInlineLevelChunkPacketIndices;
    std::unordered_map<size_t, std::vector<int>>                mInlineSubChunkPacketIndices;
    std::mutex                                                  mPendingLevelChunksMutex;
    std::unordered_multiset<ChunkPos>                           mPendingLevelChunks;
    std::unordered_set<ChunkPos>                                mCompletedLevelChunkPositions;
    std::vector<int>                                            mPendingLevelChunkIndices;
    std::unordered_set<ChunkPos>                                mSnapshotChunks;
    std::unordered_set<ChunkPos>                                mApplyingSnapshotChunks;
    std::optional<DimensionType>                                mChunkIsolationDimension;
    std::unordered_map<ChunkPos, SnapshotColumnIdentity>        mAppliedSnapshotColumns;
    std::unordered_map<ChunkPos, SnapshotColumnIdentity>        mPendingSnapshotColumns;
    std::unordered_set<ChunkPos>                                mDirtySnapshotColumns;
    std::unordered_set<ChunkPos>                                mReusableSnapshotColumns;
    std::unordered_set<ChunkPos>                                mDirectSnapshotColumns;
    std::unordered_set<ChunkPos>                                mDirectSubChunkColumns;
    std::unordered_set<int>                                     mDirectLevelChunkIndices;
    std::vector<int>                                            mPendingSubChunkIndices;
    std::vector<PendingSubChunkPacket>                          mPendingSubChunkPackets;
    std::optional<std::string>                                  mPendingSnapshotLocalPlayer;
    std::vector<std::pair<MinecraftPacketIds, std::string>>     mPendingSnapshotGamePackets;
    std::unordered_map<int32_t, std::string>                    mAppliedConfigurationPackets;
    std::unordered_set<int32_t>                                 mUnknownConfigurationPackets;
    std::unordered_set<ActorUniqueID>                           mRecordedEntityIds;
    std::optional<ActorUniqueID>                                mRecordedLocalPlayerId;
    struct ReplayPlayerRecord {
        ActorUniqueID id;
        std::string   name;
        bool          recorder{};
        // False means the identity is known but no player actor is in the current dimension.
        bool          present{};
    };
    // Copied before the first observer skin swap and restored when the follow binding drops.
    std::unique_ptr<::SerializedSkinRef> mObserverOriginalSkin;
    bool                                mObserverSkinApplied{false};
    mutable std::mutex                                  mReplayPlayersMutex;
    std::unordered_map<std::string, ReplayPlayerRecord> mReplayPlayers;
    std::string                                         mReplaySubjectChoice;
    // The subject a person camera track names; overrides the user's choice while set.
    std::optional<std::string>                          mShotSubject;
    // Choice that already logged "not in this snapshot", so a missing player does not spam every tick.
    std::string                                         mReplaySubjectGapChoice;
    // Played ticks an entity choice has had no actor. A removed entity hands the view back to the
    // recorder after a short grace; a re-add with the same id inside it binds again.
    int                                                 mReplaySubjectEntityMissingTicks{0};
    int                                                 mReplaySubjectEntityMissingLastTick{-1};
    std::unordered_map<ActorUniqueID, visuals::EntityRenderKey> mEntityRenderKeys;
    bool                                                        mLevelSimFrozen{false};
    // Item-use duration each recorded player had after the last replay tick; the client ticks it between ticks too.
    struct HeldItemUse {
        void const* item{};
        int         duration{};
    };
    std::unordered_map<ActorUniqueID, HeldItemUse> mHeldItemUse;
    std::unordered_set<std::string>                mReplayObjectiveNames;
    std::unordered_set<ChunkPos>                   mCenterChunkPositions;
    // Last replay-chunk-center log signature, so identical plans do not log again.
    std::string                                    mChunkCenterLogKey;
    std::unordered_map<ChunkPos, size_t>           mRemainingSubChunkPacketsByColumn;

    std::unordered_map<ChunkPos, std::shared_ptr<LevelChunk>> mRetainedReplayChunks;

    std::weak_ptr<MinecraftScreenModel> mScreenModel;
    Player*                             mReplayPlayer   = nullptr;
    LegacyClientNetworkHandler*         mNetworkHandler = nullptr;

    // Open/close intervals of the recorder's container screens and server forms, seen while reading actions, so a seek
    // can show the screen that is open at its landing. packetId is the packet that opened it.
    struct RecordedScreen {
        MinecraftPacketIds packetId{};
        int                id{};
        int                openTick{};
        int                closeTick{-1};
        std::string        payload;
    };
    std::vector<RecordedScreen> mRecordedScreens;

    // The screen the replay world opened for the followed player.
    struct ScreenPreview {
        MinecraftPacketIds packetId{};
        int                id{};
        int                type{};
        int                openTick{};
        int64_t            entityRaw{-1};

        [[nodiscard]] bool shows(RecordedScreen const& screen) const {
            return packetId == screen.packetId && id == screen.id && openTick == screen.openTick;
        }
    };
    std::optional<ScreenPreview> mScreenPreview;
    // Open tick of the last interval a preview was tried for, so a skipped one is not retried every tick.
    int               mTriedScreenOpenTick{-1};
    int               mScreenCloseWaitTicks{0};
    std::atomic<bool> mWaitingForScreenClose{false};
    int               mScreenPreviewTicks{0};
    int               mLastScreenSkipTick{-1};
    std::string       mLastScreenSkipAction;

    std::atomic<std::shared_ptr<ResourcePacksInfoPacket const>> mReplayResourcePacksInfo;
    std::atomic<std::shared_ptr<ResourcePackStackPacket const>> mReplayResourcePackStack;
    bool                                                        mReplayCachedResourcePacksLoaded{};
    std::optional<std::filesystem::path>                        mMountedReplayPacks;
    bool                                                        mReplayHasServerResourcePacks{false};
    bool                                                        mLoadedWithServerResourcePacksDisabled{false};
    std::filesystem::path                                       mPendingReloadReplayPath;
    std::optional<int>                                          mPendingRestoreTickAfterReload;

    std::shared_ptr<StartGamePacket const> mReplayStartGame;
    // applyRecordedBlockRegistry runs from two hooks that can fire concurrently while the replay world loads.
    std::recursive_mutex mRecordedBlockRegistryMutex;
    bool                 mRecordedBlockRegistryApplied{};

    // Decoded when the local world's item packet arrives. Reading it at replay start asserts: the packet locks
    // the thread item registry, which is empty on the main menu.
    std::mutex       mReplayItemRegistryMutex;
    std::string      mReplayItemRegistryPayload;
    std::atomic_bool mRecordedItemRegistryApplied{};
    // Observer hotbar saved while another player's HUD is shown, so the recorder's slots can be put back.
    std::unique_ptr<FollowedHudMirror> mFollowedHudMirror;

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

    struct FollowedChunkCenter {
        std::string subject;
        float       x{};
        float       z{};
    };

    // Live actor of the camera's current non-recorder subject, only when that actor is in this snapshot's dimension.
    [[nodiscard]] std::optional<FollowedChunkCenter> followedPlayerChunkCenter(DimensionType const& snapshotDimension);

    [[nodiscard]] bool prepareChunkInjectionPlan(PlaybackView const& view);

    [[nodiscard]] bool
    tryFinishChunkInjection(std::optional<std::chrono::steady_clock::time_point> catchUpDeadline = std::nullopt);

    [[nodiscard]] bool finishChunkInjection();

    [[nodiscard]] bool injectPendingLevelChunks(std::chrono::steady_clock::time_point deadline);

    [[nodiscard]] bool
    injectReadySubChunkPackets(size_t& injectedPackets, std::chrono::steady_clock::time_point deadline);

    void updateCenterChunkReadiness();

    [[nodiscard]] bool injectChunkPacket(std::string_view payload, MinecraftPacketIds packetId);

    [[nodiscard]] bool applyRequestModeLevelChunkDirect(std::string_view payload);
    [[nodiscard]] bool applyRequestModeLevelChunkData(LevelChunkPacket const& packet);

    [[nodiscard]] bool applySubChunkDirect(std::string_view payload);

    [[nodiscard]] bool
    applyGamePacket(MinecraftPacketIds packetId, std::string_view payload, bool recordedLocalPlayer = false);

    // Tracks the recorded screen state; true when the packet was consumed (open/close), false to apply it normally.
    [[nodiscard]] bool trackScreenPacket(MinecraftPacketIds packetId, std::string_view payload);

    bool dispatchUiPacket(std::shared_ptr<Packet> packet);

    void openScreenPreview(RecordedScreen const& screen);

    void closeScreenPreview(char const* reason);

    void updateScreenPreview();

    [[nodiscard]] bool hasPoppableReplayScreenOpen() const;

    // Closes any open preview/native screen and waits for SceneStack to pop it before entity-removing mutations.
    [[nodiscard]] bool ensureReplayScreenClosedForMutation(char const* reason);

    void compactPendingChunkInjectionQueue();

    [[nodiscard]] bool nextReaderForcesSnapshot() const;

    // Last record whose half-open interval [openTick, closeTick) contains tick. Unread closes stay open.
    [[nodiscard]] RecordedScreen const* recordedScreenOpenAt(int tick) const;

    // Brings the preview in line with the recorded screen after a seek landed.
    void syncScreenPreviewAfterSeek();

    [[nodiscard]] bool prepareReplayResourcePacks(std::vector<PlaybackSerializedGamePacket> const& packets);

    void releaseReplayResourcePacks();

    void prepareRecordedBlockRegistry(std::vector<PlaybackSerializedGamePacket> const& packets);

    void prepareRecordedItemRegistry(std::vector<PlaybackSerializedGamePacket> const& packets);

    size_t preloadRecordedBlockGeometry(std::vector<std::pair<std::string, CompoundTag>> const& properties);

    size_t injectRecordedBlockMaterialComponents(std::vector<std::pair<std::string, CompoundTag>> const& properties);

    [[nodiscard]] bool applyPendingSnapshotLocalPlayer();

    void invalidateSnapshotColumns(Packet const& packet);

    [[nodiscard]] bool seekChunkDeferralOpen() const;

    [[nodiscard]] bool deferSeekChunk(int index, bool levelChunk);

    [[nodiscard]] bool holdSeekBlockPacket(Packet const& packet, std::string_view payload);

    void flushSeekChunks();

    [[nodiscard]] bool releaseSeekPackets();

    void clearSeekChunkDeferral();

    [[nodiscard]] bool flushPendingSnapshotGamePackets(
        bool                                         playerListOnly,
        size_t                                       maxPackets,
        std::chrono::steady_clock::time_point const& deadline
    );

    [[nodiscard]] bool clearRecordedEntities();

    [[nodiscard]] bool refreshReplayPlayer();

    void parkReplayCameraAtPreview();

    void parkObserverAtPose(keyframe::CameraRenderState const& pose);
    void parkObserverAfterFollow();
    void releasePreviewObserver();

    void updatePausedObserverPreview();
    void moveObserverTo(::Vec3 const& feetPosition, ::Vec2 const& rotation);
    void syncPreviewObserverForTick(int targetTick, bool onlyIfMoved = false);
    void syncSeekObserver();

    // Parks the observer on the recorded player for a pending teleport or while following; true if following.
    [[nodiscard]] bool updateSubjectFollow();

    // Where the observer must sit to see through the recorded player's eyes right now.
    [[nodiscard]] std::optional<keyframe::CameraRenderState> sampleSubjectFollowState();

    // Align the server authority with the client so a paused replay isn't pulled back.
    void syncObserverServerPosition(::Vec3 const& feetPosition, ::Vec2 const& rotation);

    [[nodiscard]] bool clearReplayObjectives();

    void clearReplayData();

    void finishWorldCleanup();

    [[nodiscard]] state::editing::model::FreezeTrack previewFreezeTrackCopy() const;
    [[nodiscard]] int                                previewWorldTargetTick(int playheadTick) const;

    void beginSeek(int targetTick);

    void finishSeek();

    void settleAfterSeek();

    [[nodiscard]] bool hasPendingReplayReaderBoundary() const;

    [[nodiscard]] bool advanceReplayReader(bool stopAtEnd);

    [[nodiscard]] bool advanceReplayTick(bool stopAtEnd);

    void syncLevelSimPause(bool freeze);
    void primePausedWeatherRenderer();

    void holdItemUseBetweenTicks();

    void rememberReplayPlayer(std::string uuid, std::string name, ActorUniqueID id, bool recorder);
    void noteReplayIdentity(std::string uuid, std::string name, ActorUniqueID id, bool recorder);
    void forgetReplayIdentity(std::string const& uuid);
    void markReplayPlayerAbsent(ActorUniqueID id);
    void clearReplayPlayers(bool clearChoice);
    // Updates who is spawned. Identities from the player list stay until a player-list remove.
    void refreshReplayPlayerRoster();
    void restoreObserverSkin();
    void applyObserverSkin(::SerializedSkinRef const& skin);
    void syncFollowedPlayerHud();
    void restoreFollowedPlayerHud();
    void rebindReplaySubject();
    void refreshReplaySubjectBinding();
    // Points the follow view at the preview camera track under the playhead. Export drives its own shots.
    void updatePreviewShot();
    void clearPreviewShot();
    // Applies the hide and perspective state of the effective follow view when it changes.
    void applyFollowView();
    // Actor id the current choice names: the recorder, a player uuid or an "entity:" id.
    [[nodiscard]] std::optional<ActorUniqueID> resolveReplaySubjectId(std::string const& choice) const;
    // Game-thread snapshot of the actors the editor can pick, read by the UI under its own mutex.
    void refreshReplayEntityList();
    // Reads the followed actor's flags once per replay tick. snap drops the smoothed eye offset immediately.
    void updateReplaySubjectPosture(bool snap);

public:
    bool start(std::filesystem::path filePath);
    void stop();

    void requestStop() { mStopRequested.store(true, std::memory_order_release); }

    void requestSeek(int tick) { mRequestedSeekTick.store(tick < 0 ? 0 : tick, std::memory_order_release); }

    void tick();

    void updateControlPlane();

    [[nodiscard]] bool isActive() const { return mActive; }
    [[nodiscard]] bool isReadyForExport() const {
        return mActive && mReplayWorldJoined && mWorldReady && !mReplayFailed && mReplayPlayer && mNetworkHandler
            && !mPendingReplayDimension && !mApplyingChunkSnapshot && !mChunkInjectionPending;
    }

    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    [[nodiscard]] bool isSeekPending() const noexcept {
        return mSeekTargetTick >= 0 || mRequestedSeekTick.load(std::memory_order_acquire) >= 0;
    }

    // Lets a paused preview reapply its camera when a seek lands on the tick it already shows.
    [[nodiscard]] uint64_t seekFinishCount() const noexcept { return mSeekFinishCount.load(std::memory_order_acquire); }

    [[nodiscard]] bool hasJoinedReplayWorld() const { return mReplayWorldJoined; }

    // True once the snapshot and chunks are in and no transition is pending; gates the editor UI.
    [[nodiscard]] bool isReplayWorldReady() const {
        return mActive && mReplayWorldJoined && mWorldReady && !mReplayFailed && !mPendingReplayDimension
            && !mApplyingChunkSnapshot && !mChunkInjectionPending;
    }

    [[nodiscard]] Player* getReplayPlayer() const noexcept { return mReplayPlayer; }

    // Something outside the replay rewrites the level clock between render passes; export pins it before each pass.
    void enforceReplayWorldTime(std::optional<float> partialTickOverride = std::nullopt);

    void                                     setTimeOfDayOverride(std::optional<double> ticks) noexcept;
    void                                     setWeatherOverride(visuals::ReplayWeatherMode mode) noexcept;
    [[nodiscard]] visuals::ReplayWeatherMode weatherOverride() const noexcept { return mWeatherOverride; }
    [[nodiscard]] bool isRecordedDaylightCycleEnabled() const noexcept { return mRecordedDoDaylightCycle; }

    [[nodiscard]] bool replayHasServerResourcePacks() const noexcept { return mReplayHasServerResourcePacks; }
    [[nodiscard]] bool loadedWithServerResourcePacksDisabled() const noexcept {
        return mLoadedWithServerResourcePacksDisabled;
    }
    [[nodiscard]] std::filesystem::path const& getReplayFilePath() const noexcept { return mReplayFilePath; }
    [[nodiscard]] bool hasPendingReload() const noexcept { return !mPendingReloadReplayPath.empty(); }
    bool               requestReloadCurrentReplay(int restoreTick);

    void teleportReplayPlayer(::Vec3 const& feetPosition, ::Vec2 const& rotation);

    // Preview drives the observer (the camera) per frame at the render partial tick.

    void updateObserverPreview();

    // Chunk streaming, snapshot applies and seeks hold the last pose instead of exposing the stale observer.
    // While following, re-pins the observer just before the frame is drawn; returns the pose the camera must use.
    [[nodiscard]] std::optional<keyframe::CameraRenderState> pinFollowObserverForRender();

    // Moves the observer onto the recorded player once, as soon as the replay world and the player's pose are ready.
    void requestTeleportToRecordedPlayer() noexcept { mTeleportToSubjectPending = true; }

    // Parks the free observer at a pose once the replay world can take it (a keyframe's "move view here").
    void requestParkObserver(keyframe::CameraRenderState const& pose) { mPendingObserverPark = pose; }

    // Keeps the observer on the recorded player's first-person view until the user takes over or picks a camera.
    void setFollowRecordedPlayer(bool follow);

    // Pulls the follow camera behind the recorded player. Only honoured while
    // following; leaving the follow view (free camera, preview camera, export) always returns to first person.
    void setFollowThirdPerson(bool thirdPerson);

    [[nodiscard]] bool isFollowThirdPerson() const noexcept {
        return mFollowThirdPerson.load(std::memory_order_acquire);
    }

    // The follow view actually shown: a person camera track, or the user's follow when no track is active.
    [[nodiscard]] bool isFollowViewActive() const noexcept {
        auto const mode = mPreviewShotMode.load(std::memory_order_acquire);
        return mode == PreviewShotMode::Person
            || (mode == PreviewShotMode::None && mFollowRecordedPlayer.load(std::memory_order_acquire));
    }

    [[nodiscard]] bool isFollowViewThirdPerson() const noexcept {
        return mPreviewShotMode.load(std::memory_order_acquire) == PreviewShotMode::Person
                 ? mShotThirdPerson.load(std::memory_order_acquire)
                 : mFollowThirdPerson.load(std::memory_order_acquire);
    }

    // Pins `subject` (empty is the recorder) as the replay subject for a person camera track; nullopt hands the
    // subject back to the user's choice.
    void setShotSubject(std::optional<std::string> subject);

    // Pulls an eye pose back behind the subject like vanilla F5, stopping short of walls.
    [[nodiscard]] keyframe::CameraRenderState thirdPersonView(keyframe::CameraRenderState const& eye) const;

    // Thread-safe eye/anchor pose of any replay player or entity (`choice` empty = recorder).
    [[nodiscard]] std::optional<keyframe::CameraRenderState>
    sampleEntityAnchor(visuals::ReplaySampleTime const& time, std::string const& choice) const;

    // Clips a camera ray from `anchor` to `target` against camera-colliding blocks.
    [[nodiscard]] state::editing::model::Vec3
    clipCameraRay(state::editing::model::Vec3 const& anchor, state::editing::model::Vec3 const& target) const;

    struct ReplayPlayerInfo {
        std::string uuid;
        std::string name;
        bool        recorder{};
        bool        present{};
    };

    // Empty uuid selects the recorded local player. The choice survives a seek and a missing actor.
    void setReplaySubjectChoice(std::string uuid);

    // The subject actually bound: a person camera track's subject, else the user's choice.
    [[nodiscard]] std::string replaySubjectChoice() const;

    // The user's own choice, shown by the pickers.
    [[nodiscard]] std::string userReplaySubjectChoice() const;

    [[nodiscard]] std::vector<ReplayPlayerInfo> replayPlayers() const;

    // Choice string that follows one replay entity. Player uuids never carry this prefix.
    static constexpr std::string_view EntityChoicePrefix = "entity:";

    [[nodiscard]] static std::string entityChoice(int64_t rawId) {
        return std::string{EntityChoicePrefix} + std::to_string(rawId);
    }

    [[nodiscard]] static std::optional<int64_t> parseEntityChoice(std::string_view choice);

    struct ReplayEntityInfo {
        std::string choice; // entityChoice(id), a player's uuid, or empty for the recorder
        std::string label;
        bool        player{};
        bool        recorder{};
        bool        mob{};
        float       x{}, y{}, z{}; // feet
        float       height{};      // collision box height
        float       distance{};    // distance to the observer
    };

    // Players, mobs, and other entities in the replay, observer excluded, nearest to the observer first, as of the last
    // replay tick.
    [[nodiscard]] std::vector<ReplayEntityInfo> replayEntities() const;

    // Display label of a choice: the player's name, the entity's label, or the choice itself.
    [[nodiscard]] std::string replaySubjectLabel(std::string const& choice) const;

    [[nodiscard]] bool isFollowingRecordedPlayer() const noexcept {
        return mFollowRecordedPlayer.load(std::memory_order_acquire);
    }

    [[nodiscard]] int getCurrentTick() const {
        int const requestedTick = mRequestedSeekTick.load(std::memory_order_acquire);
        if (requestedTick >= 0) return requestedTick;
        if (mExportTimelinePhase == ReplayExportTimelinePhase::Inactive) {
            return mPreviewPlayheadTick;
        }
        return mSeekTargetTick >= 0 ? mSeekTargetTick : mCurrentTick;
    }

    // Unlike getCurrentTick(), this is never replaced by a pending seek target.
    [[nodiscard]] int getAppliedReplayTick() const { return mCurrentTick; }

    void setPreviewFreezeTrack(state::editing::model::FreezeTrack const& track);

    [[nodiscard]] bool isDimensionTransitionPending() const { return mPendingReplayDimension.has_value(); }
    [[nodiscard]] bool isWaitingForScreenClose() const noexcept {
        return mWaitingForScreenClose.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<visuals::ReplaySampleTime> getCameraRenderSampleTime() const noexcept;
    [[nodiscard]] std::optional<visuals::ReplaySampleTime> getEntityRenderSampleTime() const noexcept;
    void                                                   markReplayTickAdvanced() noexcept;
    [[nodiscard]] float                                    previewPartialTick() const noexcept;
    void                                                   resumePreviewClockFromFrozenPartial() noexcept;

    [[nodiscard]] int getTotalTicks() const;

    [[nodiscard]] std::vector<int> const& getDimensionTransitionTicks() const noexcept {
        return mDimensionTransitionTicks;
    }

    [[nodiscard]] float getPlaybackSpeed() const { return mPlaybackSpeed; }
    [[nodiscard]] float effectivePlaybackSpeed() const noexcept;
    void                setClipSpeedScale(float scale) noexcept;

    void adjustPlaybackSpeed(int direction);

    // Snaps to the nearest supported speed; the UI offers exactly playbackSpeeds().
    void                                        setPlaybackSpeed(float speed);
    [[nodiscard]] static std::span<float const> playbackSpeeds() noexcept;

    [[nodiscard]] bool setPaused(bool paused);

    [[nodiscard]] bool beginExportTimeline(int startTick);
    [[nodiscard]] bool beginExportCutSeek(int targetTick, bool forceSnapshot = true);

    void setExportCameraViewpoint(std::optional<ReplayCameraViewpoint> viewpoint) noexcept;
    [[nodiscard]] std::optional<ReplayCameraViewpoint> currentCameraViewpoint() const noexcept;
    [[nodiscard]] std::optional<ReplayCameraViewpoint> exportCameraViewpoint() const noexcept;

    void updateExportObserver(ReplayCameraViewpoint const& viewpoint);

    [[nodiscard]] std::unique_ptr<visuals::ScopedReplayEntityPose>
    createReplayEntityRenderScope(visuals::ReplaySampleTime const& sample);

    [[nodiscard]] ReplayExportTickState prepareExportTick(int targetTick);

    [[nodiscard]] bool finishExportTimelineInitialization();
    void               endExportTimeline();

    [[nodiscard]] bool isInjectingPacket(Packet const* packet) const {
        return packet && mInjectingPacket.load(std::memory_order_acquire) == packet;
    }

    [[nodiscard]] bool isIsolatingReplayWorld() const { return mActive; }

    [[nodiscard]] bool hasRecordedBlockRegistryApplied() const { return mRecordedBlockRegistryApplied; }

    [[nodiscard]] std::shared_ptr<ResourcePacksInfoPacket const> getReplayResourcePacksInfo() const {
        return mReplayResourcePacksInfo.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_ptr<ResourcePackStackPacket const> getReplayResourcePackStack() const {
        return mReplayResourcePackStack.load(std::memory_order_acquire);
    }

    void applyRecordedBlockRegistry();

    // The client builds its item registry once, from the first ItemRegistryPacket it handles. The local world's
    // packet is swapped for the recorded one; returns null when there is none or it was already handed out.
    [[nodiscard]] std::shared_ptr<ItemRegistryPacket const> takeRecordedItemRegistry();

    void logRecordedItemRegistryApplied(ItemRegistryPacket const& packet) const;

    [[nodiscard]] bool shouldIsolateChunkPackets() const;

    [[nodiscard]] bool shouldSuppressNativeChunk(ChunkPos const& pos, DimensionType packetDimension) const;

    [[nodiscard]] bool isReplayWorldCleanupPending() const { return mCleanupState != CleanupState::None; }

    [[nodiscard]] static bool isReplayLevel(Level const& level);

    void configureReplayDimension(DimensionArguments& arguments) const;

    // Whether a level id is the replay world's. Unlike isReplayLevel it may be asked from the server thread.
    [[nodiscard]] bool isReplayWorldLevelId(std::string_view levelId) const;

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

    void handleConfigurationPacket(PlaybackBuffer& data);

    void handleGamePacket(PlaybackBuffer& data);

    void handleMoveEntities(PlaybackBuffer& data);
    void handleLocalCameraOffset(PlaybackBuffer& data);
    void handleLocalViewSamples(PlaybackBuffer& data);

private:
    ReplaySession();
    ~ReplaySession();

    struct EntityAnchorMeta {
        std::optional<visuals::EntityRenderKey> renderKey;
        bool                                    isRecorder{};
        bool                                    isPlayer{};
        float                                   eyeHeight{};
        float                                   postureOffsetY{};
        float                                   fallbackX{};
        float                                   fallbackY{};
        float                                   fallbackZ{};
        float                                   fallbackYaw{};
        float                                   fallbackPitch{};
    };

    mutable std::mutex                            mEntityAnchorMutex;
    std::unordered_map<int64_t, EntityAnchorMeta> mEntityAnchors;
    std::optional<int64_t>                        mRecorderAnchorId;

    mutable std::mutex            mReplayEntitiesMutex;
    std::vector<ReplayEntityInfo> mReplayEntities;

public:
    [[nodiscard]] static ReplaySession& getInstance() {
        static ReplaySession instance;
        return instance;
    }
};

} // namespace playback::replay
