#include "FreezeTimelineEvaluator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>

namespace playback::keyframe {
namespace {

using state::editing::model::clampFreezeDelayTicks;
using state::editing::model::FreezeTrack;
using state::editing::model::MaxFreezeDelayTicks;

[[nodiscard]] double solveFreezePowerBase(int frozenDelay) noexcept {
    if (frozenDelay <= 0) return 0.0;

    long double const limit      = frozenDelay <= 5 ? 0.999L : 1.999L;
    long double const derivative = frozenDelay <= 5 ? 1.0L : 0.5L;
    long double       lo         = 0.05L;
    long double       hi         = 0.95L;
    for (int step = 0; step < 100; ++step) {
        long double const mid   = 0.5L * (lo + hi);
        long double const denom = 1.0L - std::pow(mid, static_cast<long double>(frozenDelay));
        long double const slope = denom > 0.0L ? limit * (-std::log(mid)) / denom : derivative;
        if (slope > derivative) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return static_cast<double>(0.5L * (lo + hi));
}

} // namespace

bool hasActiveFreezeTrack(FreezeTrack const& track) noexcept { return track.enabled && !track.keysByTick.empty(); }

bool hasActiveFreezeTrack(state::editing::model::EditorStateExt const& project) noexcept {
    return hasActiveFreezeTrack(project.freezeTrack);
}

double computeFreezePowerBase(int frozenDelay) noexcept {
    static std::array<double, MaxFreezeDelayTicks + 1> const cachedBases = [] {
        std::array<double, MaxFreezeDelayTicks + 1> result{};
        for (int delay = 0; delay <= MaxFreezeDelayTicks; ++delay) {
            result[static_cast<size_t>(delay)] = solveFreezePowerBase(delay);
        }
        return result;
    }();
    int const clamped = clampFreezeDelayTicks(frozenDelay);
    return cachedBases[static_cast<size_t>(clamped)];
}

FreezeEvaluation evaluateFreezeAt(FreezeTrack const& track, long double playheadTick) noexcept {
    if (!std::isfinite(playheadTick)) return {};
    if (!hasActiveFreezeTrack(track)) return {false, false, playheadTick, 0, 0};

    auto const& keys = track.keysByTick;
    if (playheadTick < static_cast<long double>(keys.begin()->first)) {
        return {false, false, playheadTick, 0, 0};
    }

    long double const floored    = std::floor(playheadTick);
    int const         lookupTick = floored >= static_cast<long double>(std::numeric_limits<int>::max())
                                     ? std::numeric_limits<int>::max()
                                     : static_cast<int>(floored);
    auto              it         = keys.upper_bound(lookupTick);
    if (it == keys.begin()) return {false, false, playheadTick, 0, 0};
    --it;

    if (!it->second.frozen) {
        return {false, false, playheadTick, 0, it->first};
    }

    // Contiguous frozen keyframes anchor to the start of the run so intermediate keyframes do not restart easing.
    auto anchor = it;
    while (anchor != keys.begin()) {
        auto const prev = std::prev(anchor);
        if (!prev->second.frozen) break;
        anchor = prev;
    }

    int const anchorTick  = anchor->first;
    int const frozenDelay = clampFreezeDelayTicks(anchor->second.frozenDelay);
    if (frozenDelay <= 0) {
        return {true, true, static_cast<long double>(anchorTick), 0, anchorTick};
    }

    long double const limit   = frozenDelay <= 5 ? 0.999L : 1.999L;
    long double const elapsed = playheadTick - static_cast<long double>(anchorTick);
    if (elapsed <= 0.0L) {
        return {true, false, static_cast<long double>(anchorTick), frozenDelay, anchorTick};
    }
    if (elapsed >= static_cast<long double>(frozenDelay)) {
        return {true, true, static_cast<long double>(anchorTick) + limit, frozenDelay, anchorTick};
    }

    long double const base  = static_cast<long double>(computeFreezePowerBase(frozenDelay));
    long double const denom = 1.0L - std::pow(base, static_cast<long double>(frozenDelay));
    long double const frac =
        denom > 0.0L ? (1.0L - std::pow(base, elapsed)) / denom : (elapsed / static_cast<long double>(frozenDelay));
    long double const cTick = static_cast<long double>(anchorTick) + limit * std::clamp(frac, 0.0L, 1.0L);
    return {true, false, cTick, frozenDelay, anchorTick};
}

FreezeEvaluation
evaluateFreezeAt(state::editing::model::EditorStateExt const& project, long double playheadTick) noexcept {
    return evaluateFreezeAt(project.freezeTrack, playheadTick);
}

std::vector<FreezeSpan> collectFreezeSpans(FreezeTrack const& track, int horizonTick) {
    std::vector<FreezeSpan> spans;
    if (!hasActiveFreezeTrack(track) || horizonTick <= 0) return spans;

    bool inFrozen  = false;
    int  spanStart = 0;
    int  spanDelay = 0;
    for (auto const& [tick, key] : track.keysByTick) {
        if (tick >= horizonTick) break;
        if (key.frozen && !inFrozen) {
            inFrozen  = true;
            spanStart = std::max(0, tick);
            spanDelay = clampFreezeDelayTicks(key.frozenDelay);
        } else if (!key.frozen && inFrozen) {
            int const spanEnd = std::clamp(tick, 0, horizonTick);
            if (spanStart < spanEnd) spans.push_back(FreezeSpan{spanStart, spanEnd, spanDelay});
            inFrozen = false;
        }
    }
    if (inFrozen && spanStart < horizonTick) {
        spans.push_back(FreezeSpan{spanStart, horizonTick, spanDelay});
    }
    return spans;
}

} // namespace playback::keyframe
