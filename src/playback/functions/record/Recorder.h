#pragma once

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/functions/packet/PlaybackSetEquipmentPacket.h"
#include "playback/functions/render/ReplayThumbnail.h"
#include "playback/utils/container/LinkedHashMap.h"

#include "mc/deps/core/utility/AutomaticID.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/platform/UUID.h"
#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class LevelChunkPacket;
class SubChunkPacket;
class Packet;

namespace playback::functions {

enum class RecordingState { Idle, Recording, Paused, Closing };

struct RecordingStatusSnapshot {
    RecordingState             state{RecordingState::Idle};
    std::chrono::seconds       elapsed{};
};

struct PacketLifecycleSemantics;

struct PlaybackView {
    float x     = 0.0f;
    float y     = 0.0f;
    float z     = 0.0f;
    float yaw   = 0.0f;
    float pitch = 0.0f;
};

struct PlaybackChunkMeta {
    int  duration          = 0;
    bool forcePlaySnapshot = false;
};

struct PlaybackMeta {
    std::string name = "Unnamed";
    std::string worldName;
    int         totalTicks = 0;

    utils::container::LinkedHashMap<std::string, PlaybackChunkMeta> chunks;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;
};

class Recorder {
private:
    enum class State { Idle, Recording, Paused, Closing };
    enum class SnapshotCaptureResult { Success, NotReady, Failed };
    struct SnapshotDimension {
        DimensionType id{};
        int32_t       minHeight{};
        int32_t       maxHeight{};
    };
    std::unique_ptr<AsyncReplaySaver> mAsyncReplaySaver;

    std::vector<std::shared_ptr<LevelChunkPacket>> mSnapshotLevelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>   mSnapshotSubChunks;
    std::vector<PlaybackSerializedGamePacket>      mSnapshotConfigurationPackets;
    std::vector<PlaybackSerializedGamePacket>      mSnapshotEntityPackets;
    std::optional<std::string>                     mSnapshotLocalPlayerPayload;
    std::vector<PlaybackSerializedGamePacket>      mConfigurationPackets;
    std::unordered_map<int32_t, size_t>             mConfigurationPacketIndices;
    std::vector<PlaybackSerializedGamePacket>      mPendingGamePackets;
    mutable std::mutex                             mPendingGamePacketsMutex;
    std::unordered_map<int32_t, uint64_t>          mRecordedGamePacketCounts;
    std::unordered_map<ActorUniqueID, std::string> mLastEntityMovements;
    std::optional<ActorUniqueID>                   mRecordedLocalPlayerId;
    std::optional<ActorRuntimeID>                  mRecordedLocalPlayerRuntimeId;
    std::optional<mce::UUID>                       mRecordedLocalPlayerUuid;
    std::optional<std::string>                     mLastLocalPlayerDataPacket;
    std::optional<PlaybackSetEquipmentPacket>      mLastLocalPlayerEquipmentPacket;
    std::optional<int>                             mLastLocalPlayerSwingTime;
    std::optional<PlaybackView>                    mSnapshotView;
    std::optional<SnapshotDimension>               mSnapshotDimension;

    std::optional<DimensionType>        mRecordingDimension;
    std::string                         mSnapshotFailure;
    std::chrono::steady_clock::duration mLongestSnapshotStall{};

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic<State>                                   mState{State::Idle};
    std::atomic<render::ReplayThumbnailCaptureProvider*> mThumbnailCaptureProvider{};
    std::atomic_bool                                     mNeedsInitialSnapshot       = true;
    std::atomic_bool                                     mDimensionTransitionPending = false;
    std::atomic<int>                                     mDimensionTransitionTargetId{0};
    std::chrono::steady_clock::time_point                mRecordingStartedAt{};
    std::chrono::steady_clock::duration                  mRecordedDuration{};

    int mChunkIndex          = 0;
    int mTicksInCurrentChunk = 0;
    int mWrittenTicks        = 0;

    bool mHasOpenChunk                  = false;
    bool mOpenChunkHasData              = false;
    bool mCurrentChunkForcePlaySnapshot = false;
    bool mThumbnailCaptureRequested     = false;

private:
    static constexpr int RECORD_CHUNK_TICKS      = 20 * 60 * 5;
    static constexpr int THUMBNAIL_CAPTURE_TICKS = 20;

    [[nodiscard]] SnapshotCaptureResult captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait);

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

    [[nodiscard]] bool writeLocalPlayerState();

    [[nodiscard]] bool finishCurrentChunk(bool close);

    void failRecording(std::string_view reason);

    void cancelRecording(std::string_view reason);

    void saveRecording();

    void logRecordedGamePacketSummary() const;

    void resetStateForNewRecording();

    void resetChunkSnapshot();

    void recordConfigurationPacket(Packet const& packet, PacketLifecycleSemantics const& semantics);

public:
    Recorder();

    [[nodiscard]] bool isActive() const {
        auto const state = mState.load();
        return state == State::Recording || state == State::Paused;
    }

    [[nodiscard]] bool isPaused() const { return mState.load() == State::Paused; }

    [[nodiscard]] RecordingStatusSnapshot getStatusSnapshot() const;

    void start();
    void pause();
    void stop();

    void setThumbnailCaptureProvider(render::ReplayThumbnailCaptureProvider* provider) {
        mThumbnailCaptureProvider.store(provider, std::memory_order_release);
    }

    void recordSpawnedActor(ActorRuntimeID runtimeId, Packet const& fallbackPacket);

    void recordNetworkGamePacket(Packet const& packet);

    void recordGamePacket(Packet const& packet);

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

} // namespace playback::functions
