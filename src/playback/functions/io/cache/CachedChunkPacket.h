#pragma once

#include <array>
#include <cstdint>

class LevelChunkPacket;

namespace playback::functions {

class CachedChunkPacket {
private:
    int                     mX;
    int                     mZ;
    std::array<uint8_t, 64> mBigHash;

public:
    int      mIndex;
    uint64_t mLongHashCode;

private:
    static std::array<uint8_t, 64> computePacketBigHash(const LevelChunkPacket& packet);

public:
    CachedChunkPacket(const LevelChunkPacket& packet, int index);
    ~CachedChunkPacket() = default;

    bool operator==(const CachedChunkPacket& other) const;
};

} // namespace playback::functions
