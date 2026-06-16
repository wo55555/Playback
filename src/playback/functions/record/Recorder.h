#pragma once

#include "playback/functions/io/AsyncReplaySaver.h"

#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class LevelChunkPacket;

namespace playback::functions {

struct PlaybackMeta {
    std::string worldName;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;
};

class Recorder {
private:
    AsyncReplaySaver mAsyncReplaySaver;

    std::unordered_map<ChunkPos, std::shared_ptr<LevelChunkPacket>> mChunkCache;
    std::mutex                                                      mChunkCacheMutex;

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic_bool mIsPaused  = false;
    std::atomic_bool mWasPaused = false;

private:
    void writeSnapshot();

    void writeChunkDataSnapshot(std::vector<std::unique_ptr<Packet>>& gamePackets);

public:
    Recorder();

    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    void start();
    void pause();
    void stop();

    void cacheChunkPacket(LevelChunkPacket& packet);

public:
    [[nodiscard]] static Recorder& getInstance() {
        static Recorder instance;
        return instance;
    }
};

class ReplayExporter {
public:
    static bool saveReplayData(std::filesystem::path const& replayPath);

    static bool writePlaybackMeta(std::filesystem::path const& replayPath, PlaybackMeta const& meta);
};

void hookNetwork(bool);

} // namespace playback::functions
