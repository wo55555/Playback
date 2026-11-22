#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <vector>

namespace playback::keyframe {

[[nodiscard]] bool hasActiveSpeedCurve(state::editing::model::EditorStateExt const& project) noexcept;

[[nodiscard]] double sampleSpeedTrackAt(state::editing::model::SpeedTrack const& track, long double replayTick);

[[nodiscard]] double effectiveSpeedAt(state::editing::model::EditorStateExt const& project, long double replayTick);

class SpeedTimeMapping {
public:
    explicit SpeedTimeMapping(state::editing::model::EditorStateExt const& project, int horizonTick = -1);

    [[nodiscard]] double unscaledTicksAt(long double replayTick) const noexcept;
    [[nodiscard]] double replayTickForUnscaled(long double unscaledTicks) const noexcept;
    [[nodiscard]] double outputSecondsBetween(int startTick, int endTick) const noexcept;
    [[nodiscard]] double totalOutputSecondsForEnabledSlices(state::editing::model::EditorStateExt const& project) const;

    [[nodiscard]] int totalTicks() const noexcept { return mTotalTicks; }

private:
    int                 mTotalTicks{};
    std::vector<double> mUnscaledAtTick;
    std::vector<double> mSegmentSpeeds;
};

} // namespace playback::keyframe
