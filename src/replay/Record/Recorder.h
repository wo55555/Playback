#pragma once

#include "replay/ReplayTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace replay::record {

struct RecorderConfig {
    CompressionKind compression{CompressionKind::None};
    bool allowSparseChunk{false};
};

struct SnapshotFrame {
    GameTick tick{};
    UnixMillis timestamp{};
    std::vector<std::byte> payload;
};

struct ActionFrame {
    GameTick tick{};
    UnixMillis timestamp{};
    std::vector<std::byte> payload;
};

struct RecorderStats {
    std::uint64_t totalSnapshots{};
    std::uint64_t totalActions{};
    ByteCount totalPayloadBytes{};
};

struct ReplayArchive {
    ReplaySession session;
    std::vector<std::byte> payloadBlob;

    [[nodiscard]] bool empty() const noexcept { return session.empty(); }
};

class Recorder {
public:
    explicit Recorder(RecorderConfig config = {});
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) noexcept;
    Recorder& operator=(Recorder&&) noexcept;

    void begin(ReplayMetadata metadata);
    [[nodiscard]] bool isRecording() const noexcept;

    bool pushSnapshot(GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload);
    bool pushAction(GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload);

    [[nodiscard]] ReplaySession finalize(UnixMillis endedAt);
    [[nodiscard]] ReplayArchive finalizeArchive(UnixMillis endedAt);
    [[nodiscard]] const RecorderStats& stats() const noexcept;

private:
    struct ChunkBuilder;

    [[nodiscard]] ChunkBuilder& ensureChunkForTick(GameTick tick);
    [[nodiscard]] bool validateFrame(GameTick tick, std::span<const std::byte> payload) const noexcept;
    void appendSnapshot(ChunkBuilder& chunk, GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload);
    void appendAction(ChunkBuilder& chunk, GameTick tick, UnixMillis timestamp, std::span<const std::byte> payload);
    void sealChunk(ChunkBuilder& chunk);

private:
    RecorderConfig mConfig{};
    ReplayMetadata mMetadata{};
    std::vector<ChunkBuilder> mChunks{};
    RecorderStats mStats{};

    bool mRecording{false};
    bool mHasTickRange{false};
    GameTick mLatestTick{};
    std::optional<GameTick> mLastSnapshotTick{};
    std::optional<ChunkIndex> mLastSnapshotChunkIndex{};
};

} // namespace replay::record
