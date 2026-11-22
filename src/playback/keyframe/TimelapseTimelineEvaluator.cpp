#include "TimelapseTimelineEvaluator.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>

namespace playback::keyframe {
namespace {

using state::editing::model::clampTimelapseOutputTicks;
using state::editing::model::MaxTimelapseOutputTicks;
using state::editing::model::MaxTimelapseSpeed;
using state::editing::model::MinTimelapseOutputTicks;
using state::editing::model::MinTimelapseSpeed;
using state::editing::model::TimelapseTrack;

[[nodiscard]] bool isSegmentValid(int startTick, int endTick, int startOutputTicks, int endOutputTicks) noexcept {
    return endTick > startTick && endOutputTicks > startOutputTicks;
}

[[nodiscard]] double
computeSegmentSpeed(int startTick, int endTick, int startOutputTicks, int endOutputTicks) noexcept {
    if (!isSegmentValid(startTick, endTick, startOutputTicks, endOutputTicks)) return 1.0;
    double const raw =
        static_cast<double>(endTick - startTick) / static_cast<double>(endOutputTicks - startOutputTicks);
    return std::clamp(raw, MinTimelapseSpeed, MaxTimelapseSpeed);
}

[[nodiscard]] std::optional<std::pair<
    std::map<int, state::editing::model::TimelapseKeyframe>::const_iterator,
    std::map<int, state::editing::model::TimelapseKeyframe>::const_iterator>>
findSegmentAt(TimelapseTrack const& track, long double replayTick) noexcept {
    if (!track.enabled || track.keysByTick.size() < 2 || !std::isfinite(replayTick)) return std::nullopt;
    auto const& keys = track.keysByTick;
    if (replayTick < static_cast<long double>(keys.begin()->first)
        || replayTick >= static_cast<long double>(keys.rbegin()->first)) {
        return std::nullopt;
    }
    auto const upper = keys.upper_bound(static_cast<int>(std::floor(replayTick)));
    if (upper == keys.begin() || upper == keys.end()) return std::nullopt;
    auto const left  = std::prev(upper);
    auto const right = upper;
    if (!isSegmentValid(left->first, right->first, left->second.outputTicks, right->second.outputTicks)) {
        return std::nullopt;
    }
    return std::pair{left, right};
}

[[nodiscard]] std::string_view trimWhitespace(std::string_view view) noexcept {
    auto const isSpace = [](unsigned char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!view.empty() && isSpace(static_cast<unsigned char>(view.front()))) view.remove_prefix(1);
    while (!view.empty() && isSpace(static_cast<unsigned char>(view.back()))) view.remove_suffix(1);
    return view;
}

[[nodiscard]] std::optional<int64_t> parseNonNegativeInt(std::string_view token) noexcept {
    if (token.empty()) return std::nullopt;
    for (char c : token) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    int64_t    value = 0;
    auto const res   = std::from_chars(token.data(), token.data() + token.size(), value);
    if (res.ec != std::errc{} || res.ptr != token.data() + token.size() || value < 0) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<double> parseNonNegativeDecimal(std::string_view token) noexcept {
    if (token.empty()) return std::nullopt;
    bool hasDigit = false;
    bool hasDot   = false;
    for (char c : token) {
        if (c >= '0' && c <= '9') {
            hasDigit = true;
        } else if (c == '.') {
            if (hasDot) return std::nullopt;
            hasDot = true;
        } else {
            return std::nullopt;
        }
    }
    if (!hasDigit) return std::nullopt;
    double     value = 0.0;
    auto const res   = std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::fixed);
    if (res.ec != std::errc{} || res.ptr != token.data() + token.size() || !std::isfinite(value) || value < 0.0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<int> secondsToClampedTicks(double seconds) noexcept {
    if (!std::isfinite(seconds) || seconds < 0.0) return std::nullopt;
    double const raw = seconds * 20.0;
    if (!std::isfinite(raw)) return std::nullopt;
    if (raw >= static_cast<double>(MaxTimelapseOutputTicks)) return MaxTimelapseOutputTicks;
    long long const rounded = std::llround(raw);
    return clampTimelapseOutputTicks(
        static_cast<int>(std::clamp<long long>(rounded, MinTimelapseOutputTicks, MaxTimelapseOutputTicks))
    );
}

} // namespace

std::vector<TimelapseSegment> collectTimelapseSegments(TimelapseTrack const& track) {
    std::vector<TimelapseSegment> segments;
    if (track.keysByTick.size() < 2) return segments;
    segments.reserve(track.keysByTick.size() - 1);
    for (auto left = track.keysByTick.begin(), right = std::next(left); right != track.keysByTick.end();
         left = right, ++right) {
        int const    startTick   = left->first;
        int const    endTick     = right->first;
        int const    startOutput = left->second.outputTicks;
        int const    endOutput   = right->second.outputTicks;
        bool const   valid       = isSegmentValid(startTick, endTick, startOutput, endOutput);
        double const speed       = computeSegmentSpeed(startTick, endTick, startOutput, endOutput);
        segments.push_back(TimelapseSegment{startTick, endTick, startOutput, endOutput, valid, speed});
    }
    return segments;
}

bool hasValidTimelapseSegment(TimelapseTrack const& track) noexcept {
    if (track.keysByTick.size() < 2) return false;
    for (auto left = track.keysByTick.begin(), right = std::next(left); right != track.keysByTick.end();
         left = right, ++right) {
        if (isSegmentValid(left->first, right->first, left->second.outputTicks, right->second.outputTicks)) {
            return true;
        }
    }
    return false;
}

bool hasActiveTimelapseTrack(TimelapseTrack const& track) noexcept {
    return track.enabled && hasValidTimelapseSegment(track);
}

bool hasActiveTimelapseTrack(state::editing::model::EditorStateExt const& project) noexcept {
    return hasActiveTimelapseTrack(project.timelapseTrack);
}

std::optional<double> sampleTimelapseSpeedAt(TimelapseTrack const& track, long double replayTick) noexcept {
    auto const seg = findSegmentAt(track, replayTick);
    if (!seg) return std::nullopt;
    auto const [left, right] = *seg;
    return computeSegmentSpeed(left->first, right->first, left->second.outputTicks, right->second.outputTicks);
}

TimelapseEvaluation evaluateTimelapseAt(TimelapseTrack const& track, long double replayTick) noexcept {
    auto const seg = findSegmentAt(track, replayTick);
    if (!seg) return {};
    auto const [left, right] = *seg;
    int const    startTick   = left->first;
    int const    endTick     = right->first;
    int const    deltaOut    = right->second.outputTicks - left->second.outputTicks;
    double const speed = computeSegmentSpeed(startTick, endTick, left->second.outputTicks, right->second.outputTicks);
    return TimelapseEvaluation{true, speed, startTick, endTick, deltaOut};
}

TimelapseEvaluation
evaluateTimelapseAt(state::editing::model::EditorStateExt const& project, long double replayTick) noexcept {
    return evaluateTimelapseAt(project.timelapseTrack, replayTick);
}

int suggestTimelapseOutputTicks(TimelapseTrack const& track, int tick) noexcept {
    if (track.keysByTick.empty()) return 0;
    if (auto const exact = track.keysByTick.find(tick); exact != track.keysByTick.end()) {
        return clampTimelapseOutputTicks(exact->second.outputTicks);
    }

    auto const lower = track.keysByTick.lower_bound(tick);
    auto const upper = track.keysByTick.upper_bound(tick);

    if (lower == track.keysByTick.begin()) {
        int const nextTick = upper->first;
        int const nextOut  = upper->second.outputTicks;
        return clampTimelapseOutputTicks(std::max(0, nextOut - std::max(20, (nextTick - tick) / 10)));
    }

    auto const prev = std::prev(lower);
    if (upper == track.keysByTick.end()) {
        int const prevTick = prev->first;
        int const prevOut  = prev->second.outputTicks;
        return clampTimelapseOutputTicks(prevOut + std::max(20, (tick - prevTick) / 10));
    }

    int const prevTick = prev->first;
    int const nextTick = upper->first;
    int const prevOut  = prev->second.outputTicks;
    int const nextOut  = upper->second.outputTicks;
    if (nextOut > prevOut + 1) {
        double const ratio  = static_cast<double>(tick - prevTick) / static_cast<double>(nextTick - prevTick);
        int const    interp = prevOut + static_cast<int>(std::llround(ratio * static_cast<double>(nextOut - prevOut)));
        return clampTimelapseOutputTicks(std::clamp(interp, prevOut + 1, nextOut - 1));
    }
    return clampTimelapseOutputTicks(prevOut + 1);
}

std::optional<int> parseTimelapseDurationTicks(std::string_view text) noexcept {
    std::string_view const trimmed = trimWhitespace(text);
    if (trimmed.empty()) return std::nullopt;

    if (trimmed.back() == 't' || trimmed.back() == 'T') {
        auto const num = parseNonNegativeInt(trimWhitespace(trimmed.substr(0, trimmed.size() - 1)));
        if (!num) return std::nullopt;
        return clampTimelapseOutputTicks(
            static_cast<int>(std::clamp<int64_t>(*num, MinTimelapseOutputTicks, MaxTimelapseOutputTicks))
        );
    }

    if (trimmed.find(':') != std::string_view::npos) {
        std::string_view parts[3];
        size_t           partCount = 0;
        size_t           start     = 0;
        while (true) {
            size_t const pos = trimmed.find(':', start);
            if (partCount >= 3) return std::nullopt;
            if (pos == std::string_view::npos) {
                parts[partCount++] = trimmed.substr(start);
                break;
            }
            parts[partCount++] = trimmed.substr(start, pos - start);
            start              = pos + 1;
        }
        if (partCount == 2) {
            auto const mm = parseNonNegativeInt(parts[0]);
            auto const ss = parseNonNegativeDecimal(parts[1]);
            if (!mm || !ss || *ss >= 60.0) return std::nullopt;
            return secondsToClampedTicks(static_cast<double>(*mm) * 60.0 + *ss);
        }
        if (partCount == 3) {
            auto const hh = parseNonNegativeInt(parts[0]);
            auto const mm = parseNonNegativeInt(parts[1]);
            auto const ss = parseNonNegativeDecimal(parts[2]);
            if (!hh || !mm || !ss || *mm >= 60 || *ss >= 60.0) return std::nullopt;
            return secondsToClampedTicks(static_cast<double>(*hh) * 3600.0 + static_cast<double>(*mm) * 60.0 + *ss);
        }
        return std::nullopt;
    }

    bool hasAlpha = false;
    for (char c : trimmed) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            hasAlpha = true;
            break;
        }
    }

    if (hasAlpha) {
        double totalSeconds = 0.0;
        size_t idx          = 0;
        bool   parsedAny    = false;
        while (idx < trimmed.size()) {
            while (idx < trimmed.size() && (trimmed[idx] == ' ' || trimmed[idx] == '\t')) ++idx;
            if (idx >= trimmed.size()) break;

            size_t const numStart = idx;
            while (idx < trimmed.size() && ((trimmed[idx] >= '0' && trimmed[idx] <= '9') || trimmed[idx] == '.')) {
                ++idx;
            }
            if (idx == numStart || idx >= trimmed.size()) return std::nullopt;
            auto const val = parseNonNegativeDecimal(trimmed.substr(numStart, idx - numStart));
            if (!val) return std::nullopt;

            char const unit = trimmed[idx++];
            double     mult = 0.0;
            if (unit == 'h' || unit == 'H') mult = 3600.0;
            else if (unit == 'm' || unit == 'M') mult = 60.0;
            else if (unit == 's' || unit == 'S') mult = 1.0;
            else return std::nullopt;

            totalSeconds += (*val) * mult;
            parsedAny     = true;
        }
        if (!parsedAny) return std::nullopt;
        return secondsToClampedTicks(totalSeconds);
    }

    auto const sec = parseNonNegativeDecimal(trimmed);
    if (!sec) return std::nullopt;
    return secondsToClampedTicks(*sec);
}

} // namespace playback::keyframe
