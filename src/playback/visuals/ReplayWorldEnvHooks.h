#pragma once

#include <cstdint>
#include <optional>

namespace playback::visuals {

enum class ReplayWeatherMode : uint8_t {
    Recorded = 0,
    Clear    = 1,
    Overcast = 2,
    Rain     = 3,
    Snow     = 4,
    Thunder  = 5,
};

bool hookReplayWorldEnv(bool enable);
void setReplayWorldEnvActive(bool active) noexcept;
void setReplayWorldTimeState(int recordedTimeTicks, bool daylightCycleRunning, float partialTick) noexcept;
void setReplayWorldTimeOverride(std::optional<double> overrideTimeTicks) noexcept;
void setReplayWeatherOverride(ReplayWeatherMode mode) noexcept;
[[nodiscard]] ReplayWeatherMode getReplayWeatherOverride() noexcept;

} // namespace playback::visuals
