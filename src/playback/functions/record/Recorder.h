#pragma once

#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/utils/container/LinkedHashMap.h"

#include "mc/world/level/ChunkPos.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

class LevelChunkPacket;

namespace playback::functions {

struct PlaybackMeta {
    std::string name = "Unnamed";
    std::string worldName;

    utils::container::LinkedHashMap<std::string, PlaybackMeta> chunks;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;
};

class Recorder {
private:
    AsyncReplaySaver mAsyncReplaySaver;

    std::unordered_map<ChunkPos, std::shared_ptr<LevelChunkPacket>> mChunkCache;
    std::mutex                                                      mChunkCacheMutex;

    PlaybackMeta mMetadata = PlaybackMeta();

    std::atomic_bool mIsPaused    = false;
    std::atomic_bool mWasPaused   = false;
    std::atomic_bool mIsRecording = false;

private:
    void writeSnapshot();

    void writeChunkDataSnapshot(std::vector<std::unique_ptr<Packet>>& gamePackets);

public:
    Recorder();

    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    void start();
    void pause();
    void stop();

    void recordTickPacket();

    void cacheChunkPacket(LevelChunkPacket& packet);

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
