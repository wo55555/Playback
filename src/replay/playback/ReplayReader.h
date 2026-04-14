#pragma once

#include "replay/ReplayTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace replay::playback {

enum class ReplayFrameKind : std::uint8_t {
    Snapshot = 0,
    Action = 1,
};

struct ReplayFrame {
    ReplayFrameKind kind{ReplayFrameKind::Action};
    GameTick tick{};
    UnixMillis timestamp{};
    std::vector<std::byte> payload;
};

struct DecodedChunk {
    ReplayChunk metadata;
    std::vector<ReplayFrame> frames;
};

class ChunkDeserializer {
public:
    [[nodiscard]] static std::optional<DecodedChunk>
    decode(const ReplayChunk& metadata, std::span<const std::byte> encodedChunk);
};

class ReplayReader {
public:
    ReplayReader(ReplaySession session, std::vector<std::byte> payloadBlob);

    [[nodiscard]] bool buildTimeline();
    [[nodiscard]] bool seekTick(GameTick tick) noexcept;
    [[nodiscard]] std::optional<ReplayFrame> nextFrame();

    [[nodiscard]] bool hasNext() const noexcept;
    [[nodiscard]] std::size_t frameCount() const noexcept;
    void reset() noexcept;

private:
    [[nodiscard]] std::span<const std::byte> locateChunkBytes(const ReplayChunk& chunk) const noexcept;

private:
    ReplaySession mSession;
    std::vector<std::byte> mPayloadBlob;
    std::vector<ReplayFrame> mTimeline;
    std::size_t mCursor{};
};

} // namespace replay::playback
