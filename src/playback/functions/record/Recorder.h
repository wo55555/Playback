#pragma once

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/utils/container/LinkedHashMap.h"

#include "mc/deps/core/utility/AutomaticID.h"
#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class LevelChunkPacket;
class SubChunkPacket;

namespace playback::functions {

struct PlaybackView {
    float x     = 0.0f;
    float y     = 0.0f;
    float z     = 0.0f;
    float yaw   = 0.0f;
    float pitch = 0.0f;
};

struct PlaybackMeta {
    std::string name = "Unnamed";
    std::string worldName;
    int         duration   = 0;
    int         totalTicks = 0;

    std::optional<PlaybackView> initialView;

    utils::container::LinkedHashMap<std::string, PlaybackMeta> chunks;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;
};

class Recorder {
private:
    enum class State { Idle, Recording, Paused, Closing };
    std::unique_ptr<AsyncReplaySaver> mAsyncReplaySaver;

    std::vector<std::shared_ptr<LevelChunkPacket>> mSnapshotLevelChunks;
    std::vector<std::shared_ptr<SubChunkPacket>>   mSnapshotSubChunks;
    std::optional<PlaybackView>                    mSnapshotView;
    std::optional<PlaybackView>                    mOpenChunkView;

    std::optional<DimensionType>        mRecordingDimension;
    std::string                         mSnapshotFailure;
    std::chrono::steady_clock::duration mLongestSnapshotStall{};

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic<State> mState{State::Idle};
    std::atomic_bool   mNeedsInitialSnapshot = true;

    int mChunkIndex          = 0;
    int mTicksInCurrentChunk = 0;
    int mWrittenTicks        = 0;

    bool mHasOpenChunk     = false;
    bool mOpenChunkHasData = false;

private:
    static constexpr int RECORD_CHUNK_TICKS = 20 * 60 * 5;

    [[nodiscard]] bool captureChunkSnapshot(std::chrono::steady_clock::duration& barrierWait);

    [[nodiscard]] bool commitChunkSnapshot(
        std::chrono::steady_clock::duration captureElapsed,
        std::chrono::steady_clock::duration barrierWait
    );

    [[nodiscard]] bool writeInitialSnapshotIfNeeded();

    [[nodiscard]] bool writeSnapshot();

    [[nodiscard]] bool writeTickBoundary();

    [[nodiscard]] bool finishCurrentChunk(bool close);

    void failRecording(std::string_view reason);

    void cancelRecording(std::string_view reason);

    void saveRecording();

    void resetStateForNewRecording();

    void resetChunkSnapshot();

public:
    Recorder();

    [[nodiscard]] bool isPaused() const { return mState.load() == State::Paused; }

    void start();
    void pause();
    void stop();

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
