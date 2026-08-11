#pragma once

#include "playback/functions/render/ReplaySampleTime.h"

#include <cstdint>
#include <optional>

namespace playback::editor::exporting {

struct OfflineRenderClockSample {
    functions::render::ReplaySampleTime replayTime;
    float                               deltaTicks{};
    int                                 wholeTicks{};
    uint64_t                            frameIndex{};
};

struct OfflineRenderClockToken {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

enum class OfflineRenderClockPublishResult : uint8_t { Published, Unavailable, Busy, InvalidSample };

struct OfflineRenderBoundaryTicket {
    uint64_t    clockToken{};
    uint64_t    frameIndex{};
    uint64_t    renderSerial{};
    void const* bgfxFrame{};
    uint32_t    bgfxFrameNumber{};
    uint32_t    gameRenderOrdinal{};
};

[[nodiscard]] bool hookOfflineRenderClock(bool enable);
[[nodiscard]] bool isOfflineRenderClockInstalled();

[[nodiscard]] OfflineRenderClockPublishResult
                   publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token);
[[nodiscard]] bool wasOfflineRenderClockSampleApplied(OfflineRenderClockToken token);
[[nodiscard]] bool wasOfflineRenderClockSampleCompleted(OfflineRenderClockToken token);
// Context::swap records the exact API frame belonging to the active sample.
// The renderer thread may claim only the matching submit; unrelated BGFX
// submissions must not consume the export ticket.
[[nodiscard]] std::optional<OfflineRenderBoundaryTicket>
claimOfflineRenderBoundary(void const* frame, uint32_t frameNumber);
// Present remains a compatibility fallback for renderers without the explicit
// D3D12 BGFX submission path.
[[nodiscard]] std::optional<OfflineRenderBoundaryTicket> claimOfflineRenderPresentFallback();
void markOfflineRenderBoundaryCompleted(OfflineRenderBoundaryTicket const& ticket);
void clearOfflineRenderClockSample(OfflineRenderClockToken token);
void resetOfflineRenderClock();

} // namespace playback::editor::exporting
