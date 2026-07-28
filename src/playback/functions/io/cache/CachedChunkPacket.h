#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace playback::functions {

class CachedChunkPacket {
private:
    std::array<uint8_t, 64> mBigHash;

public:
    int      mIndex;
    uint64_t mLongHashCode;

private:
    static std::array<uint8_t, 64> computePacketBigHash(int32_t packetId, std::string_view payload);

public:
    CachedChunkPacket(int32_t packetId, std::string_view payload, int index);
    ~CachedChunkPacket() = default;

    bool operator==(const CachedChunkPacket& other) const;
};

} // namespace playback::functions
