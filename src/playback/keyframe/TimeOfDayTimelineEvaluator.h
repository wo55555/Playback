#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <optional>

namespace playback::keyframe {

[[nodiscard]] bool hasActiveTimeOfDayTrack(state::editing::model::EditorStateExt const& project) noexcept;

[[nodiscard]] std::optional<double>
evaluateTimeOfDayAt(state::editing::model::TimeOfDayTrack const& track, double tick) noexcept;

[[nodiscard]] std::optional<double>
resolveEffectiveTimeOfDay(state::editing::model::EditorStateExt const& project, double tick) noexcept;

} // namespace playback::keyframe
