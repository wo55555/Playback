#pragma once

#include "playback/functions/render/ReplaySampleTime.h"

#include <cstdint>

namespace playback::editor::exporting {

struct OfflineRenderClockSample {
    functions::render::ReplaySampleTime replayTime;
    float                               deltaTicks{};
    int                                 wholeTicks{};
};

struct OfflineRenderClockToken {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

enum class OfflineRenderClockPublishResult : uint8_t { Published, Unavailable, Busy, InvalidSample };

[[nodiscard]] bool hookOfflineRenderClock(bool enable);
[[nodiscard]] bool isOfflineRenderClockInstalled();

[[nodiscard]] OfflineRenderClockPublishResult
                   publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token);
[[nodiscard]] bool wasOfflineRenderClockSampleApplied(OfflineRenderClockToken token);
void               clearOfflineRenderClockSample(OfflineRenderClockToken token);
void               resetOfflineRenderClock();

} // namespace playback::editor::exporting
