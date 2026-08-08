#pragma once

#include <cstdint>

namespace playback::editor::exporting {

struct OfflineRenderClockSample {
    uint64_t frameIndex{};
    float    partialTick{};
    float    deltaTicks{};
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
[[nodiscard]] bool didOfflineRenderClockSampleFail(OfflineRenderClockToken token);
void               clearOfflineRenderClockSample(OfflineRenderClockToken token);
void               resetOfflineRenderClock();

} // namespace playback::editor::exporting
