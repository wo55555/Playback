#pragma once

#include "playback/visuals/ReplaySampleTime.h"

#include <cstdint>

namespace playback::exporting {

// What a render step is for. A capture step renders once, into the armed frame capture; the others render every frame
// until they are cleared.
enum class OfflineRenderClockKind : uint8_t { KeepAlive, Capture, Warmup, Convergence };

struct OfflineRenderClockSample {
    visuals::ReplaySampleTime replayTime;
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

// What the client acceleration did to the game's sim timer; all zero when it was never installed.
struct OfflineRenderClientBoostStats {
    uint64_t advances{};
    uint64_t pendingAdvances{};
    uint64_t naturalTicks{};
    uint64_t forcedTicks{};
    uint64_t otherTimerAdvances{};
    float    minPassedAfterForce{};
    float    maxPassedAfterForce{};
};

[[nodiscard]] bool hookOfflineRenderClock(bool enable);
[[nodiscard]] bool isOfflineRenderClockInstalled();

// Hands out a native client tick as soon as one is requested instead of waiting for the 20 Hz timer.
// Removed together with the other export hooks by hookOfflineRenderClock(false).
[[nodiscard]] bool                          setOfflineRenderClientBoost(bool enable);
[[nodiscard]] OfflineRenderClientBoostStats takeOfflineRenderClientBoostStats();

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
