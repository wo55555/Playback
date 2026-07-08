#include "CachedChunkPacket.h"

#include "mc/network/packet/LevelChunkPacket.h"

#include "openssl/evp.h"
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace playback::functions {

CachedChunkPacket::CachedChunkPacket(const LevelChunkPacket& packet, int index)
: mX(packet.mPos->x),
  mZ(packet.mPos->z),
  mBigHash(computePacketBigHash(packet)),
  mIndex(index) {
    mLongHashCode = XXH3_64bits(mBigHash.data(), mBigHash.size());
}

std::array<uint8_t, 64> CachedChunkPacket::computePacketBigHash(const LevelChunkPacket& packet) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }

    const auto md = EVP_sha3_512();

    EVP_DigestInit_ex(ctx, md, nullptr);

    EVP_DigestUpdate(ctx, &packet.mPos->x, sizeof(packet.mPos->x));
    EVP_DigestUpdate(ctx, &packet.mPos->z, sizeof(packet.mPos->z));
    const auto dimensionId = *packet.mDimensionId;
    EVP_DigestUpdate(ctx, &dimensionId, sizeof(dimensionId));
    EVP_DigestUpdate(ctx, packet.mSerializedChunk->data(), packet.mSerializedChunk->size());

    unsigned char hashBuf[EVP_MAX_MD_SIZE];
    unsigned int  hashLen = 0;
    EVP_DigestFinal_ex(ctx, hashBuf, &hashLen);

    EVP_MD_CTX_free(ctx);

    if (hashLen != 64) {
        throw std::runtime_error("Hash length mismatch (expected 64 bytes)");
    }

    std::array<uint8_t, 64> result{};
    std::copy(hashBuf, hashBuf + 64, result.begin());
    return result;
}

bool CachedChunkPacket::operator==(const CachedChunkPacket& other) const {
    if (this->mLongHashCode != other.mLongHashCode) return false;
    if (this->mX != other.mX || this->mZ != other.mZ) return false;
    return mBigHash == other.mBigHash;
}

} // namespace playback::functions
