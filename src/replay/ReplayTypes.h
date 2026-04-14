#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay {

using UnixMillis = std::int64_t;
using GameTick = std::int64_t;
using ChunkIndex = std::uint32_t;
using ByteCount = std::uint64_t;
using ContentHash = std::uint64_t;

enum class ReplayFormatVersion : std::uint32_t {
    V1 = 1,
};

enum class ChunkPayloadKind : std::uint8_t {
    Snapshot = 0,
    Incremental = 1,
    Hybrid = 2,
};

enum class CompressionKind : std::uint8_t {
    None = 0,
    Lz4 = 1,
    Zstd = 2,
};

struct ReplayChunk {
    ChunkIndex index{};
    GameTick startTick{};
    GameTick endTick{};

    ChunkPayloadKind payloadKind{ChunkPayloadKind::Hybrid};
    std::optional<ChunkIndex> baseSnapshotChunkIndex{};

    std::uint32_t snapshotCount{};
    std::uint32_t actionCount{};

    ByteCount payloadOffset{};
    ByteCount payloadSize{};
    CompressionKind compression{CompressionKind::None};
    ContentHash contentHash{};

    [[nodiscard]] bool hasSnapshot() const noexcept { return snapshotCount != 0; }
    [[nodiscard]] bool hasActions() const noexcept { return actionCount != 0; }
    [[nodiscard]] bool isValid() const noexcept { return endTick >= startTick; }
};

struct ReplayMetadata {
    ReplayFormatVersion formatVersion{ReplayFormatVersion::V1};

    std::string replayId;
    std::string displayName = "Unnamed";
    std::string gameVersion;
    std::string modVersion;
    std::string levelName;
    std::string dimensionId;
    std::string recorderName;
    std::string recorderXuid;
    std::string recorderUuid;

    UnixMillis createdAt{};
    UnixMillis startedAt{};
    UnixMillis endedAt{};

    GameTick firstTick{};
    GameTick lastTick{};
    GameTick snapshotIntervalTicks{20 * 5};
    GameTick chunkDurationTicks{20 * 30};

    std::optional<std::uint64_t> worldSeed{};
    std::uint32_t totalChunkCount{};
    ByteCount totalBytes{};
};

struct ReplaySession {
    ReplayMetadata metadata;
    std::vector<ReplayChunk> chunks;

    [[nodiscard]] bool empty() const noexcept { return chunks.empty(); }
    [[nodiscard]] std::size_t chunkCount() const noexcept { return chunks.size(); }
    [[nodiscard]] GameTick durationTicks() const noexcept {
        return metadata.lastTick >= metadata.firstTick ? metadata.lastTick - metadata.firstTick : 0;
    }
};

} // namespace replay