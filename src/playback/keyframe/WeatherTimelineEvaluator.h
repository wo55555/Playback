#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <optional>

namespace playback::keyframe {

[[nodiscard]] bool hasActiveWeatherTrack(state::editing::model::WeatherTrack const& track) noexcept;
[[nodiscard]] bool hasActiveWeatherTrack(state::editing::model::EditorStateExt const& project) noexcept;

[[nodiscard]] std::optional<state::editing::model::WeatherOverride>
evaluateWeatherAt(state::editing::model::WeatherTrack const& track, double tick) noexcept;

[[nodiscard]] state::editing::model::WeatherOverride
resolveEffectiveWeather(state::editing::model::EditorStateExt const& project, double tick) noexcept;

} // namespace playback::keyframe
