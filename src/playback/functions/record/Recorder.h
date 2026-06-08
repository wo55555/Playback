#pragma once

#include "mc/network/Packet.h"
#include "mc/world/level/ChunkPos.h"
#include "mc/world/level/dimension/Dimension.h"

#include <atomic>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace playback::functions {

struct PlaybackMeta {
    std::string worldName;

    static PlaybackMeta       fromJson(std::string_view json);
    [[nodiscard]] std::string toJson() const;
};

class Recorder {
private:
    struct ChunkPacketData {
        ::ChunkPos      pos;
        ::DimensionType dimId;
        uint64          subChunksCount;
        std::string     serializedChunk;
    };

    std::unordered_map<::ChunkPos, ChunkPacketData> mChunkCache;
    std::mutex                                      mChunkCacheMutex;

    std::atomic_bool mIsPaused  = false;
    std::atomic_bool mWasPaused = false;

private:
    void writeChunkDataSnapshot(std::list<std::unique_ptr<Packet>>& gamePackets);

public:
    [[nodiscard]] bool isPaused() const { return mIsPaused; }

    void start();
    void pause();
    void stop();

    /// @brief 缓存来自网络的原生 LevelChunk 数据包，供快照时使用
    void cacheChunkPacket(::ChunkPos pos, ::DimensionType dimId, uint64 subChunksCount, std::string&& serializedChunk);

private:
    Recorder() = default;

public:
    [[nodiscard]] static Recorder& getInstance() {
        static Recorder instance;
        return instance;
    }
};

void hookNetwork(bool);

} // namespace playback::functions
