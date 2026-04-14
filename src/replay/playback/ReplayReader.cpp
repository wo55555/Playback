#include "replay/playback/ReplayReader.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace replay::playback {
namespace {

constexpr std::uint8_t kChunkEncodingVersion = 1;
constexpr std::uint8_t kFrameKindSnapshot = 0;
constexpr std::uint8_t kFrameKindAction = 1;
constexpr std::array<char, 8> kChunkMagic{'R', 'C', 'H', 'N', 'K', '0', '0', '1'};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> bytes) : mBytes(bytes) {}

    template <typename T>
    bool readPod(T& out) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (remaining() < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, mBytes.data() + mOffset, sizeof(T));
        mOffset += sizeof(T);
        return true;
    }

    [[nodiscard]] bool readBytes(std::size_t size, std::span<const std::byte>& out) {
        if (remaining() < size) {
            return false;
        }
        out = mBytes.subspan(mOffset, size);
        mOffset += size;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return mBytes.size() - mOffset; }

private:
    std::span<const std::byte> mBytes;
    std::size_t mOffset{};
};

} // namespace

std::optional<DecodedChunk> ChunkDeserializer::decode(const ReplayChunk& metadata, std::span<const std::byte> encodedChunk) {
    BinaryReader reader(encodedChunk);

    std::span<const std::byte> magicBytes{};
    if (!reader.readBytes(kChunkMagic.size(), magicBytes)) {
        return std::nullopt;
    }

    if (!std::equal(magicBytes.begin(), magicBytes.end(), reinterpret_cast<const std::byte*>(kChunkMagic.data()))) {
        return std::nullopt;
    }

    std::uint8_t version{};
    if (!reader.readPod(version) || version != kChunkEncodingVersion) {
        return std::nullopt;
    }

    ChunkIndex index{};
    std::uint32_t snapshotCount{};
    std::uint32_t actionCount{};
    GameTick startTick{};
    GameTick endTick{};
    if (!reader.readPod(index) || !reader.readPod(snapshotCount) || !reader.readPod(actionCount) || !reader.readPod(startTick)
        || !reader.readPod(endTick)) {
        return std::nullopt;
    }

    if (index != metadata.index || startTick != metadata.startTick || endTick != metadata.endTick) {
        return std::nullopt;
    }

    DecodedChunk decoded{};
    decoded.metadata = metadata;
    decoded.frames.reserve(static_cast<std::size_t>(snapshotCount) + static_cast<std::size_t>(actionCount));

    std::uint32_t parsedSnapshots{};
    std::uint32_t parsedActions{};

    while (reader.remaining() > 0) {
        std::uint8_t frameKind{};
        GameTick tick{};
        UnixMillis timestamp{};
        std::uint32_t payloadSize{};

        if (!reader.readPod(frameKind) || !reader.readPod(tick) || !reader.readPod(timestamp) || !reader.readPod(payloadSize)) {
            return std::nullopt;
        }

        std::span<const std::byte> payloadBytes{};
        if (!reader.readBytes(payloadSize, payloadBytes)) {
            return std::nullopt;
        }

        ReplayFrame frame{};
        frame.tick = tick;
        frame.timestamp = timestamp;
        frame.payload.assign(payloadBytes.begin(), payloadBytes.end());

        if (frameKind == kFrameKindSnapshot) {
            frame.kind = ReplayFrameKind::Snapshot;
            parsedSnapshots += 1;
        } else if (frameKind == kFrameKindAction) {
            frame.kind = ReplayFrameKind::Action;
            parsedActions += 1;
        } else {
            return std::nullopt;
        }

        decoded.frames.push_back(std::move(frame));
    }

    if (parsedSnapshots != snapshotCount || parsedActions != actionCount) {
        return std::nullopt;
    }

    return decoded;
}

ReplayReader::ReplayReader(ReplaySession session, std::vector<std::byte> payloadBlob)
: mSession(std::move(session)),
  mPayloadBlob(std::move(payloadBlob)) {}

std::span<const std::byte> ReplayReader::locateChunkBytes(const ReplayChunk& chunk) const noexcept {
    const auto offset = static_cast<std::size_t>(chunk.payloadOffset);
    const auto size = static_cast<std::size_t>(chunk.payloadSize);
    if (offset > mPayloadBlob.size() || size > mPayloadBlob.size() - offset) {
        return {};
    }
    return std::span<const std::byte>(mPayloadBlob.data() + offset, size);
}

bool ReplayReader::buildTimeline() {
    mTimeline.clear();
    mCursor = 0;

    if (mSession.chunks.empty()) {
        return true;
    }

    for (const auto& chunk : mSession.chunks) {
        const auto encoded = locateChunkBytes(chunk);
        if (encoded.empty()) {
            return false;
        }

        auto decoded = ChunkDeserializer::decode(chunk, encoded);
        if (!decoded.has_value()) {
            return false;
        }

        for (auto& frame : decoded->frames) {
            mTimeline.push_back(std::move(frame));
        }
    }

    std::stable_sort(mTimeline.begin(), mTimeline.end(), [](const ReplayFrame& lhs, const ReplayFrame& rhs) {
        return lhs.tick < rhs.tick;
    });

    return true;
}

bool ReplayReader::seekTick(GameTick tick) noexcept {
    const auto it = std::lower_bound(mTimeline.begin(), mTimeline.end(), tick, [](const ReplayFrame& frame, GameTick value) {
        return frame.tick < value;
    });

    mCursor = static_cast<std::size_t>(std::distance(mTimeline.begin(), it));
    return mCursor < mTimeline.size();
}

std::optional<ReplayFrame> ReplayReader::nextFrame() {
    if (!hasNext()) {
        return std::nullopt;
    }

    return mTimeline[mCursor++];
}

bool ReplayReader::hasNext() const noexcept { return mCursor < mTimeline.size(); }

std::size_t ReplayReader::frameCount() const noexcept { return mTimeline.size(); }

void ReplayReader::reset() noexcept { mCursor = 0; }

} // namespace replay::playback
