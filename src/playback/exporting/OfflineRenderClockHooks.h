#pragma once

#include "playback/visuals/ReplaySampleTime.h"

#include <cstdint>
#include <optional>

namespace playback::exporting {

// What a render step is for. A capture step renders once, into the armed frame capture; warm-up and extra render steps
// render every frame until they are cleared. KeepAlive is never published: it marks the redraws of the last render
// that fill the frames with no step to render.
enum class OfflineRenderClockKind : uint8_t { KeepAlive, Capture, Warmup, ExtraRender };

struct OfflineRenderClockSample {
    visuals::ReplaySampleTime replayTime;
    visuals::ReplaySampleTime clientTime;
    // Time the step adds for the game timer and the renderers' clock. It passes with the step's first render; any
    // further render of the step draws the same instant again and adds none.
    float                     deltaTicks{};
    int                       wholeTicks{};
    uint64_t                  frameIndex{};
    OfflineRenderClockKind    kind{OfflineRenderClockKind::KeepAlive};
};

struct OfflineRenderClockToken {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

enum class OfflineRenderClockPublishResult : uint8_t { Published, Unavailable, Busy, InvalidSample };

[[nodiscard]] bool hookOfflineRenderClock(bool enable);
[[nodiscard]] bool isOfflineRenderClockInstalled();

// Call right before the client update. A client tick the export is waiting for then runs in that update instead of
// whenever the game's 20 Hz timer next comes due, so the frame that needs it can render in the same pass of the loop.
void grantPendingOfflineReplayTick();

[[nodiscard]] OfflineRenderClockPublishResult
     publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token);
void markOfflineRenderClockCaptureArmed(OfflineRenderClockToken token);
// Re-renders a capture sample whose scene submit was early or never arrived; false when nothing changed.
[[nodiscard]] bool retryMissedOfflineRenderScene(OfflineRenderClockToken token);
[[nodiscard]] bool wasOfflineRenderClockSampleApplied(OfflineRenderClockToken token);
// Overlay-only BGFX submissions carry no world geometry and must never satisfy an armed capture.
enum class SceneSubmissionKind : uint8_t { OverlayOnly, Scene };

void clearOfflineRenderClockSample(OfflineRenderClockToken token);
void resetOfflineRenderClock();


} // namespace playback::exporting
