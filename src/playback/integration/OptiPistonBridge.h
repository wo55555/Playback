#pragma once

#include <cstdint>
#include <optional>

namespace playback::integration {

// Optional OptiPiston link; every call is a no-op when the mod is absent.
struct OptiPistonSettings {
    bool  enabled{};
    float durationTicks{};
    float minDurationTicks{};
    float maxDurationTicks{};
};

[[nodiscard]] bool                              isOptiPistonLoaded() noexcept;
[[nodiscard]] std::optional<OptiPistonSettings> optiPistonSettings() noexcept;
void                                            setOptiPistonEnabled(bool enabled) noexcept;
void                                            setOptiPistonDuration(float ticks) noexcept;

// Drives OptiPiston from the replay tick; jumped marks a seek or restart.
void pushReplayClock(int64_t tick, bool jumped) noexcept;
// Replay frame fraction; empty returns to the native render alpha.
void pushReplayPartial(std::optional<float> partial) noexcept;
void clearReplayClock() noexcept;

} // namespace playback::integration
