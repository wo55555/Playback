#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <vector>

namespace playback::keyframe {

struct FreezeEvaluation {
    bool        frozen{false};
    bool        worldFrozen{false};
    long double clientTick{0.0L};
    int         frozenDelay{0};
    int         anchorTick{0};
};

struct FreezeSpan {
    int startTick{};
    int endTick{};
    int frozenDelay{0};
};

[[nodiscard]] bool hasActiveFreezeTrack(state::editing::model::FreezeTrack const& track) noexcept;
[[nodiscard]] bool hasActiveFreezeTrack(state::editing::model::EditorStateExt const& project) noexcept;

[[nodiscard]] double computeFreezePowerBase(int frozenDelay) noexcept;

[[nodiscard]] FreezeEvaluation
evaluateFreezeAt(state::editing::model::FreezeTrack const& track, long double playheadTick) noexcept;

[[nodiscard]] FreezeEvaluation
evaluateFreezeAt(state::editing::model::EditorStateExt const& project, long double playheadTick) noexcept;

[[nodiscard]] std::vector<FreezeSpan>
collectFreezeSpans(state::editing::model::FreezeTrack const& track, int horizonTick);

} // namespace playback::keyframe
