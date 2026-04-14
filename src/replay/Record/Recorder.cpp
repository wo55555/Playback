#include "replay/Record/Recorder.h"

#include <algorithm>
#include <array>
#include <utility>

namespace replay::record {
namespace {

constexpr std::uint8_t kChunkEncodingVersion = 1;
constexpr std::uint8_t kFrameKindSnapshot = 0;
constexpr std::uint8_t kFrameKindAction = 1;
constexpr std::array<char, 8> kChunkMagic{'R', 'C', 'H', 'N', 'K', '0', '0', '1'};

class BinaryWriter {
public:
    void writeBytes(std::span<const std::byte> bytes) {
        mBuffer.insert(mBuffer.end(), bytes.begin(), bytes.end());
    }

    template <typename T>
    void writePod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* begin = reinterpret_cast<const std::byte*>(std::addressof(value));
        writeBytes({begin, sizeof(T)});
    }

    void writeFrame(std::uint8_t kind, GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload) {
        const auto payloadSize = static_cast<std::uint32_t>(payload.size());
        writePod(kind);
        writePod(tick);
        writePod(timestamp);
        writePod(payloadSize);
        writeBytes(payload);
    }

    [[nodiscard]] std::vector<std::byte> takeBuffer() { return std::move(mBuffer); }

private:
    std::vector<std::byte> mBuffer{};
};

[[nodiscard]] ContentHash fnv1a64(std::span<const std::byte> data) noexcept {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;

    std::uint64_t hash = kOffsetBasis;
    for (const auto byte : data) {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= kPrime;
    }
    return hash;
}

} // namespace

struct Recorder::ChunkBuilder {
    ReplayChunk chunk{};
    BinaryWriter writer{};
    std::vector<std::byte> encodedChunk{};
    bool sealed{false};
};

Recorder::Recorder(RecorderConfig config) : mConfig(config) {}

Recorder::~Recorder() = default;

Recorder::Recorder(Recorder&&) noexcept = default;

Recorder& Recorder::operator=(Recorder&&) noexcept = default;

void Recorder::begin(ReplayMetadata metadata) {
    mMetadata = std::move(metadata);
    mChunks.clear();
    mStats = {};
    mRecording = true;
    mHasTickRange = false;
    mLatestTick = 0;
    mLastSnapshotTick.reset();
    mLastSnapshotChunkIndex.reset();

    if (mMetadata.chunkDurationTicks <= 0) {
        mMetadata.chunkDurationTicks = 20 * 30;
    }
    if (mMetadata.snapshotIntervalTicks <= 0) {
        mMetadata.snapshotIntervalTicks = 20 * 5;
    }

    mMetadata.totalChunkCount = 0;
    mMetadata.totalBytes = 0;
}

bool Recorder::isRecording() const noexcept { return mRecording; }

bool Recorder::validateFrame(GameTick tick, std::span<const std::byte> payload) const noexcept {
    if (!mRecording) {
        return false;
    }
    if (payload.empty()) {
        return false;
    }
    if (mHasTickRange && tick < mMetadata.firstTick) {
        return false;
    }
    return true;
}

Recorder::ChunkBuilder& Recorder::ensureChunkForTick(GameTick tick) {
    if (!mHasTickRange) {
        mMetadata.firstTick = tick;
        mMetadata.lastTick = tick;
        mLatestTick = tick;
        mHasTickRange = true;
    }

    mMetadata.lastTick = std::max(mMetadata.lastTick, tick);
    mLatestTick = std::max(mLatestTick, tick);

    const auto relativeTick = static_cast<std::uint64_t>(tick - mMetadata.firstTick);
    const auto duration = static_cast<std::uint64_t>(mMetadata.chunkDurationTicks);
    const auto chunkIdx64 = relativeTick / duration;
    const auto chunkIdx = static_cast<ChunkIndex>(chunkIdx64);

    if (mChunks.empty()) {
        mChunks.reserve(8);
    }

    if (mConfig.allowSparseChunk) {
        auto it = std::find_if(mChunks.begin(), mChunks.end(), [chunkIdx](const ChunkBuilder& candidate) {
            return candidate.chunk.index == chunkIdx;
        });
        if (it == mChunks.end()) {
            mChunks.push_back(ChunkBuilder{});
            it = std::prev(mChunks.end());
            it->chunk.index = chunkIdx;
        }
        return *it;
    }

    while (mChunks.size() <= chunkIdx) {
        ChunkBuilder next{};
        next.chunk.index = static_cast<ChunkIndex>(mChunks.size());
        mChunks.push_back(std::move(next));
    }

    return mChunks[chunkIdx];
}

void Recorder::appendSnapshot(
    ChunkBuilder&               chunk,
    GameTick                    tick,
    UnixMillis                  timestamp,
    std::span<const std::byte>  payload
) {
    chunk.writer.writeFrame(kFrameKindSnapshot, tick, timestamp, payload);

    chunk.chunk.snapshotCount += 1;
    chunk.chunk.startTick = chunk.chunk.snapshotCount == 1 && chunk.chunk.actionCount == 0
        ? tick
        : std::min(chunk.chunk.startTick, tick);
    chunk.chunk.endTick = std::max(chunk.chunk.endTick, tick);

    mStats.totalSnapshots += 1;
    mStats.totalPayloadBytes += static_cast<ByteCount>(payload.size());
    mLastSnapshotTick = tick;
    mLastSnapshotChunkIndex = chunk.chunk.index;
}

void Recorder::appendAction(
    ChunkBuilder&               chunk,
    GameTick                    tick,
    UnixMillis                  timestamp,
    std::span<const std::byte>  payload
) {
    chunk.writer.writeFrame(kFrameKindAction, tick, timestamp, payload);

    chunk.chunk.actionCount += 1;
    chunk.chunk.startTick = chunk.chunk.snapshotCount == 0 && chunk.chunk.actionCount == 1
        ? tick
        : std::min(chunk.chunk.startTick, tick);
    chunk.chunk.endTick = std::max(chunk.chunk.endTick, tick);

    mStats.totalActions += 1;
    mStats.totalPayloadBytes += static_cast<ByteCount>(payload.size());
}

bool Recorder::pushSnapshot(GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload) {
    if (!validateFrame(tick, payload)) {
        return false;
    }

    auto& chunk = ensureChunkForTick(tick);
    appendSnapshot(chunk, tick, timestamp, payload);
    return true;
}

bool Recorder::pushAction(GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload) {
    if (!validateFrame(tick, payload)) {
        return false;
    }

    auto& chunk = ensureChunkForTick(tick);

    if (mLastSnapshotTick.has_value() && tick - *mLastSnapshotTick >= mMetadata.snapshotIntervalTicks) {
        chunk.chunk.payloadKind = ChunkPayloadKind::Hybrid;
    }

    appendAction(chunk, tick, timestamp, payload);
    return true;
}

void Recorder::sealChunk(ChunkBuilder& chunk) {
    if (chunk.sealed) {
        return;
    }

    auto payload = chunk.writer.takeBuffer();

    BinaryWriter envelope{};
    envelope.writeBytes({reinterpret_cast<const std::byte*>(kChunkMagic.data()), kChunkMagic.size()});
    envelope.writePod(kChunkEncodingVersion);
    envelope.writePod(chunk.chunk.index);
    envelope.writePod(chunk.chunk.snapshotCount);
    envelope.writePod(chunk.chunk.actionCount);
    envelope.writePod(chunk.chunk.startTick);
    envelope.writePod(chunk.chunk.endTick);
    envelope.writeBytes(payload);

    chunk.encodedChunk = envelope.takeBuffer();
    chunk.chunk.payloadSize = static_cast<ByteCount>(chunk.encodedChunk.size());
    chunk.chunk.contentHash = fnv1a64(chunk.encodedChunk);

    if (chunk.chunk.snapshotCount > 0 && chunk.chunk.actionCount == 0) {
        chunk.chunk.payloadKind = ChunkPayloadKind::Snapshot;
    } else if (chunk.chunk.snapshotCount == 0 && chunk.chunk.actionCount > 0) {
        chunk.chunk.payloadKind = ChunkPayloadKind::Incremental;
    } else {
        chunk.chunk.payloadKind = ChunkPayloadKind::Hybrid;
    }

    if (chunk.chunk.payloadKind == ChunkPayloadKind::Incremental && mLastSnapshotChunkIndex.has_value()) {
        chunk.chunk.baseSnapshotChunkIndex = mLastSnapshotChunkIndex;
    }

    chunk.chunk.compression = mConfig.compression;
    chunk.sealed = true;
}

const RecorderStats& Recorder::stats() const noexcept { return mStats; }

ReplaySession Recorder::finalize(UnixMillis endedAt) {
    return finalizeArchive(endedAt).session;
}

ReplayArchive Recorder::finalizeArchive(UnixMillis endedAt) {
    ReplayArchive archive{};
    if (!mRecording) {
        return archive;
    }

    mMetadata.endedAt = endedAt;
    if (!mHasTickRange) {
        mMetadata.firstTick = 0;
        mMetadata.lastTick = 0;
    }

    std::sort(mChunks.begin(), mChunks.end(), [](const ChunkBuilder& lhs, const ChunkBuilder& rhs) {
        return lhs.chunk.index < rhs.chunk.index;
    });

    ByteCount runningOffset = 0;
    archive.session.chunks.reserve(mChunks.size());
    for (auto& builder : mChunks) {
        sealChunk(builder);
        builder.chunk.payloadOffset = runningOffset;
        runningOffset += builder.chunk.payloadSize;
        archive.session.chunks.push_back(builder.chunk);
    }

    archive.payloadBlob.reserve(static_cast<std::size_t>(runningOffset));
    for (const auto& builder : mChunks) {
        archive.payloadBlob.insert(archive.payloadBlob.end(), builder.encodedChunk.begin(), builder.encodedChunk.end());
    }

    mMetadata.totalChunkCount = static_cast<std::uint32_t>(archive.session.chunks.size());
    mMetadata.totalBytes = runningOffset;
    archive.session.metadata = mMetadata;

    mRecording = false;
    return archive;
}

} // namespace replay::record
