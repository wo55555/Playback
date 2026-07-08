#pragma once

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/utils/container/LinkedHashMap.h"

#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class LevelChunkPacket;
class Packet;

namespace playback::functions {

struct PlaybackMeta {
    std::string name = "Unnamed";
    std::string worldName;
    int         duration   = 0;
    int         totalTicks = 0;

    utils::container::LinkedHashMap<std::string, PlaybackMeta> chunks;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;
};

class Recorder {
private:
    enum class State { Idle, Recording, Paused, Closing };
    enum class PacketPhase { Game };

    struct PacketWithPhase {
        std::shared_ptr<Packet> packet;
        PacketPhase             phase = PacketPhase::Game;
    };

    std::unique_ptr<AsyncReplaySaver> mAsyncReplaySaver;

    std::unordered_map<ChunkPos, std::shared_ptr<LevelChunkPacket>> mChunkCache;
    std::mutex                                                      mChunkCacheMutex;

    std::queue<PacketWithPhase> mPendingPackets;
    std::mutex                  mPendingPacketsMutex;

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic<State> mState{State::Idle};
    std::atomic_bool   mNeedsInitialSnapshot = true;
    std::atomic_bool   mWasPaused            = false;

    int mChunkIndex          = 0;
    int mTicksInCurrentChunk = 0;
    int mWrittenTicks        = 0;

    bool mHasOpenChunk     = false;
    bool mOpenChunkHasData = false;
    bool mFinishedPausing  = false;

private:
    static constexpr int RECORD_CHUNK_TICKS = 20 * 60 * 5;

    [[nodiscard]] bool readyToWrite() const;

    void resetStateForNewRecording();

    void writeInitialSnapshotIfNeeded();

    void writeSnapshot();

    void writeChunkDataSnapshot(std::vector<std::shared_ptr<Packet>>& gamePackets);

    bool flushPendingPackets();

    void writeTickBoundary();

    void finishCurrentChunk(bool close);

public:
    Recorder();

    [[nodiscard]] bool isPaused() const { return mState.load() == State::Paused; }

    void start();
    void pause();
    void stop();

    void recordGamePacket(std::shared_ptr<Packet> packet);

    void cacheChunkPacket(LevelChunkPacket const& packet);

    void clearChunkCache();

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

void hookNetwork(bool);

} // namespace playback::functions
