#include "TimeOfDayTimelineEvaluator.h"

#include "InterpolationHelpers.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace playback::keyframe {

using namespace detail;
using state::editing::model::clampTimeOfDayDouble;
using state::editing::model::clampTimeOfDayTicks;
using state::editing::model::TimeOfDayTrack;

bool hasActiveTimeOfDayTrack(state::editing::model::EditorStateExt const& project) noexcept {
    return project.timeOfDayTrack.enabled && !project.timeOfDayTrack.keysByTick.empty();
}

std::optional<double> evaluateTimeOfDayAt(TimeOfDayTrack const& track, double tick) noexcept {
    if (!track.enabled || track.keysByTick.empty() || !std::isfinite(tick)) return std::nullopt;

    auto const&  keys      = track.keysByTick;
    auto const   front     = keys.begin();
    auto const   back      = std::prev(keys.end());
    double const firstTick = static_cast<double>(front->first);
    double const lastTick  = static_cast<double>(back->first);
    if (tick <= firstTick) return clampTimeOfDayDouble(static_cast<double>(front->second.timeOfDay));
    if (tick >= lastTick) return clampTimeOfDayDouble(static_cast<double>(back->second.timeOfDay));

    auto const right = keys.lower_bound(static_cast<int>(std::ceil(tick)));
    if (right == keys.end()) return clampTimeOfDayDouble(static_cast<double>(back->second.timeOfDay));
    if (static_cast<double>(right->first) == tick) {
        return clampTimeOfDayDouble(static_cast<double>(right->second.timeOfDay));
    }

    auto const   left     = std::prev(right);
    auto const&  leftKey  = left->second;
    auto const&  rightKey = right->second;
    double const span     = static_cast<double>(right->first - left->first);
    float const  amount   = span <= 0.0 ? 0.0f : clampUnit(static_cast<float>((tick - left->first) / span));
    double const a        = clampTimeOfDayDouble(static_cast<double>(leftKey.timeOfDay));
    double const b        = clampTimeOfDayDouble(static_cast<double>(rightKey.timeOfDay));

    switch (leftKey.interpolationType) {
    case CameraInterpolationType::Hold:
        return a;
    case CameraInterpolationType::EaseIn: {
        float const adjusted =
            interpolateSides(CameraSidedInterpolationType::Ease, CameraSidedInterpolationType::Linear, amount);
        return clampTimeOfDayDouble(a + (b - a) * static_cast<double>(adjusted));
    }
    case CameraInterpolationType::EaseOut: {
        float const adjusted =
            interpolateSides(CameraSidedInterpolationType::Linear, CameraSidedInterpolationType::Ease, amount);
        return clampTimeOfDayDouble(a + (b - a) * static_cast<double>(adjusted));
    }
    case CameraInterpolationType::EaseInOut: {
        float const adjusted =
            interpolateSides(CameraSidedInterpolationType::Ease, CameraSidedInterpolationType::Ease, amount);
        return clampTimeOfDayDouble(a + (b - a) * static_cast<double>(adjusted));
    }
    case CameraInterpolationType::CubicBezier: {
        float const adjusted = cubicBezierEase(amount, leftKey.bezierCtrl1, leftKey.bezierCtrl2);
        return clampTimeOfDayDouble(a + (b - a) * static_cast<double>(adjusted));
    }
    case CameraInterpolationType::Smooth: {
        auto before = left;
        if (left != keys.begin()) before = std::prev(left);
        auto after = right;
        if (right != keys.end() && std::next(right) != keys.end()) after = std::next(right);
        if (before != left && before->second.interpolationType == CameraInterpolationType::Hold) before = left;
        if (after != right && right->second.interpolationType == CameraInterpolationType::Hold) after = right;

        float const p0    = static_cast<float>(clampTimeOfDayDouble(static_cast<double>(before->second.timeOfDay)));
        float const p1    = static_cast<float>(a);
        float const p2    = static_cast<float>(b);
        float const p3    = static_cast<float>(clampTimeOfDayDouble(static_cast<double>(after->second.timeOfDay)));
        float const time1 = static_cast<float>(left->first - before->first);
        float const time2 = static_cast<float>(right->first - before->first);
        float const time3 = static_cast<float>(after->first - before->first);
        return clampTimeOfDayDouble(
            static_cast<double>(centripetalCatmullRom(p0, p1, p2, p3, time1, time2, time3, amount))
        );
    }
    case CameraInterpolationType::Hermite: {
        try {
            auto runStart = left;
            while (runStart != keys.begin()
                   && std::prev(runStart)->second.interpolationType != CameraInterpolationType::Hold) {
                --runStart;
            }
            auto runEnd = left;
            ++runEnd;
            while (runEnd != keys.end() && runEnd->second.interpolationType != CameraInterpolationType::Hold) {
                ++runEnd;
            }

            std::vector<int>   ticks;
            std::vector<float> values;
            size_t const       count = static_cast<size_t>(std::distance(runStart, runEnd));
            ticks.reserve(count);
            values.reserve(count);
            for (auto it = runStart; it != runEnd; ++it) {
                ticks.push_back(it->first);
                values.push_back(static_cast<float>(clampTimeOfDayDouble(static_cast<double>(it->second.timeOfDay))));
            }
            return clampTimeOfDayDouble(
                static_cast<double>(interpolatePolynomial(ticks, values, static_cast<long double>(tick)))
            );
        } catch (...) {
            return clampTimeOfDayDouble(a + (b - a) * static_cast<double>(amount));
        }
    }
    case CameraInterpolationType::Linear:
    default:
        return clampTimeOfDayDouble(a + (b - a) * static_cast<double>(amount));
    }
}

std::optional<double>
resolveEffectiveTimeOfDay(state::editing::model::EditorStateExt const& project, double tick) noexcept {
    if (hasActiveTimeOfDayTrack(project)) {
        return evaluateTimeOfDayAt(project.timeOfDayTrack, tick);
    }
    if (project.daytimeOverrideEnabled) {
        return static_cast<double>(clampTimeOfDayTicks(project.daytimeOverrideTicks));
    }
    return std::nullopt;
}

} // namespace playback::keyframe
