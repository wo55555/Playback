#include "WeatherTimelineEvaluator.h"

#include <cmath>
#include <limits>

namespace playback::keyframe {

using state::editing::model::EditorStateExt;
using state::editing::model::WeatherOverride;
using state::editing::model::WeatherTrack;

bool hasActiveWeatherTrack(WeatherTrack const& track) noexcept { return track.enabled && !track.keysByTick.empty(); }

bool hasActiveWeatherTrack(EditorStateExt const& project) noexcept {
    return hasActiveWeatherTrack(project.weatherTrack);
}

std::optional<WeatherOverride> evaluateWeatherAt(WeatherTrack const& track, double tick) noexcept {
    if (!hasActiveWeatherTrack(track) || !std::isfinite(tick)) return std::nullopt;

    auto const& keys = track.keysByTick;
    if (tick < static_cast<double>(keys.begin()->first)) return std::nullopt;

    double const floored    = std::floor(tick);
    int const    lookupTick = floored >= static_cast<double>(std::numeric_limits<int>::max())
                                ? std::numeric_limits<int>::max()
                                : static_cast<int>(floored);
    auto         it         = keys.upper_bound(lookupTick);
    if (it == keys.begin()) return std::nullopt;
    --it;
    return it->second.weather;
}

WeatherOverride resolveEffectiveWeather(EditorStateExt const& project, double tick) noexcept {
    if (auto const weather = evaluateWeatherAt(project.weatherTrack, tick)) {
        return *weather;
    }
    return project.weatherOverride;
}

} // namespace playback::keyframe
