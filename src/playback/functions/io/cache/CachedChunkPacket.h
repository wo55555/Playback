#pragma once

#include <array>
#include <cstdint>

class Packet;

namespace playback::functions {

class CachedChunkPacket {
private:
    std::array<uint8_t, 64> mBigHash;

public:
    int      mIndex;
    uint64_t mLongHashCode;

private:
    static std::array<uint8_t, 64> computePacketBigHash(Packet const& packet);

public:
    CachedChunkPacket(Packet const& packet, int index);
    ~CachedChunkPacket() = default;

    bool operator==(const CachedChunkPacket& other) const;
};

} // namespace playback::functions
