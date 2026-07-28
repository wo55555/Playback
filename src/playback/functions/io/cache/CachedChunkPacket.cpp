#include "CachedChunkPacket.h"

#include "openssl/evp.h"
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace playback::functions {

CachedChunkPacket::CachedChunkPacket(int32_t packetId, std::string_view payload, int index)
: mBigHash(computePacketBigHash(packetId, payload)),
  mIndex(index) {
    mLongHashCode = XXH3_64bits(mBigHash.data(), mBigHash.size());
}

std::array<uint8_t, 64> CachedChunkPacket::computePacketBigHash(int32_t packetId, std::string_view payload) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }

    const auto md = EVP_sha3_512();

    EVP_DigestInit_ex(ctx, md, nullptr);

    EVP_DigestUpdate(ctx, &packetId, sizeof(packetId));
    EVP_DigestUpdate(ctx, payload.data(), payload.size());

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
    return mBigHash == other.mBigHash;
}

} // namespace playback::functions
