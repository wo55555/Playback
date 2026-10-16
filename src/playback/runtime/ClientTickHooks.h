#pragma once

#include <cstdint>
#include <optional>

namespace playback::runtime {

struct OfflineReplayTickToken {
    uint64_t id{};

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

struct OfflineReplayTickCompletion {
    uint64_t token{};
    int      replayTickBefore{};
    int      replayTickAfter{};
    bool     clientTickExecuted{};

    [[nodiscard]] int replayTicksAdvanced() const { return replayTickAfter - replayTickBefore; }
};

enum class OfflineReplayTickRequestResult : uint8_t { Requested, Unavailable, Busy };

// How long requested ticks waited for the game's own 20 Hz timer to hand them to the gate.
struct OfflineReplayTickPacing {
    uint64_t requests{};
    uint64_t claims{};
    uint64_t claimLatencyMicros{};
    uint64_t maxClaimLatencyMicros{};
};

[[nodiscard]] bool hookClientTick(bool enable);

// Runs one native level tick from inside a replay tick; false when no client level tick is in progress.
bool runNativeLevelTick();

[[nodiscard]] bool beginOfflineReplayTickGate();
void               endOfflineReplayTickGate();

[[nodiscard]] OfflineReplayTickRequestResult             requestOfflineReplayTick(OfflineReplayTickToken& token);
[[nodiscard]] bool                                       wasOfflineReplayTickCompleted(OfflineReplayTickToken token);
[[nodiscard]] std::optional<OfflineReplayTickCompletion> getOfflineReplayTickCompletion(OfflineReplayTickToken token);

// True while a requested tick has not yet reached the level tick hook.
[[nodiscard]] bool hasPendingOfflineReplayTick();
// Returns the pacing counters gathered since the last call and resets them.
[[nodiscard]] OfflineReplayTickPacing takeOfflineReplayTickPacing();

} // namespace playback::runtime
