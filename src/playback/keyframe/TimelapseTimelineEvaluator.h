#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <optional>
#include <string_view>
#include <vector>

namespace playback::keyframe {

struct TimelapseSegment {
    int    startTick{};
    int    endTick{};
    int    startOutputTicks{};
    int    endOutputTicks{};
    bool   valid{false};
    double speed{1.0};
};

struct TimelapseEvaluation {
    bool   active{false};
    double speed{1.0};
    int    segmentStartTick{-1};
    int    segmentEndTick{-1};
    int    outputDeltaTicks{0};
};

[[nodiscard]] std::vector<TimelapseSegment>
collectTimelapseSegments(state::editing::model::TimelapseTrack const& track);

[[nodiscard]] bool hasValidTimelapseSegment(state::editing::model::TimelapseTrack const& track) noexcept;

[[nodiscard]] bool hasActiveTimelapseTrack(state::editing::model::TimelapseTrack const& track) noexcept;
[[nodiscard]] bool hasActiveTimelapseTrack(state::editing::model::EditorStateExt const& project) noexcept;

[[nodiscard]] std::optional<double>
sampleTimelapseSpeedAt(state::editing::model::TimelapseTrack const& track, long double replayTick) noexcept;

[[nodiscard]] TimelapseEvaluation
evaluateTimelapseAt(state::editing::model::TimelapseTrack const& track, long double replayTick) noexcept;

[[nodiscard]] TimelapseEvaluation
evaluateTimelapseAt(state::editing::model::EditorStateExt const& project, long double replayTick) noexcept;

[[nodiscard]] int suggestTimelapseOutputTicks(state::editing::model::TimelapseTrack const& track, int tick) noexcept;

[[nodiscard]] std::optional<int> parseTimelapseDurationTicks(std::string_view text) noexcept;

} // namespace playback::keyframe
