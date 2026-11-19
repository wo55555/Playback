#include "ExportPlanCompiler.h"

#include "playback/state/editing/ClipOps.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_set>

namespace playback::exporting {

namespace {

constexpr int64_t  ReplayTicksPerSecond = 20;
constexpr int64_t  MaxFrameRateValue    = 1'000'000;
constexpr uint64_t MaxExportFrames      = 1'000'000;
constexpr uint32_t MaxResolution        = MaxExportResolution;
constexpr uint32_t MaxSsaa              = MaxExportSsaa;
constexpr uint32_t MaxWarmupFrames      = 3'600;
constexpr uint64_t MaxFramePixels       = MaxExportPixels;

[[nodiscard]] int64_t gcd(int64_t left, int64_t right) {
    while (right != 0) {
        int64_t const remainder = left % right;
        left                    = right;
        right                   = remainder;
    }
    return left == 0 ? 1 : left;
}

[[nodiscard]] bool checkedMultiply(uint64_t left, uint64_t right, uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
    result = left * right;
    return true;
}

[[nodiscard]] bool checkedAdd(uint64_t left, uint64_t right, uint64_t& result) {
    if (right > std::numeric_limits<uint64_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool checkedSignedMultiply(int64_t left, int64_t right, int64_t& result) {
    if (left == 0 || right == 0) {
        result = 0;
        return true;
    }
    if (left == -1 && right == std::numeric_limits<int64_t>::min()) return false;
    if (right == -1 && left == std::numeric_limits<int64_t>::min()) return false;
    if (left > 0 && right > 0 && left > std::numeric_limits<int64_t>::max() / right) return false;
    if (left < 0 && right < 0 && left < std::numeric_limits<int64_t>::max() / right) return false;
    if (left > 0 && right < 0 && right < std::numeric_limits<int64_t>::min() / left) return false;
    if (left < 0 && right > 0 && left < std::numeric_limits<int64_t>::min() / right) return false;
    result = left * right;
    return true;
}

[[nodiscard]] bool checkedSignedAdd(int64_t left, int64_t right, int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        || (right < 0 && left < std::numeric_limits<int64_t>::min() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool validOutputName(std::string const& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        return false;
    for (unsigned char const character : name) {
        if (std::iscntrl(character) || character == '/' || character == '\\' || character == ':' || character == '*'
            || character == '?' || character == '"' || character == '<' || character == '>' || character == '|') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasMp4Extension(std::string const& name) {
    if (name.size() < 4) return false;
    auto const offset = name.size() - 4;
    return name[offset] == '.' && std::tolower(static_cast<unsigned char>(name[offset + 1])) == 'm'
        && std::tolower(static_cast<unsigned char>(name[offset + 2])) == 'p' && name[offset + 3] == '4';
}

[[nodiscard]] FrameRate normalizeFrameRate(FrameRate rate) {
    if (rate.numerator <= 0 || rate.denominator <= 0) return rate;
    auto const divisor  = gcd(rate.numerator, rate.denominator);
    rate.numerator     /= divisor;
    rate.denominator   /= divisor;
    return rate;
}

[[nodiscard]] std::string outputBaseName(std::string name) {
    if (hasMp4Extension(name)) name.resize(name.size() - 4);
    return name;
}

[[nodiscard]] std::filesystem::path utf8Path(std::string const& value) {
    auto const* begin = reinterpret_cast<char8_t const*>(value.data());
    return std::filesystem::path(std::u8string{begin, begin + value.size()});
}

[[nodiscard]] ExportPlanCompileResult failure(ExportError error, char const* message) {
    return ExportPlanCompileResult{std::nullopt, error, message};
}

struct ExportSlice {
    int64_t inTick{};
    int64_t outTick{};
    float   speed{1.0f};
    bool    muteAudio{false};
    bool    hardCutBefore{false};
};

[[nodiscard]] int64_t quantizeSpeedMilli(float speed) noexcept {
    return std::clamp<int64_t>(
        std::llround(static_cast<double>(state::editing::model::clampClipSpeed(speed)) * 1000.0),
        50,
        32000
    );
}

void appendOrMergeSlice(
    std::vector<ExportSlice>& slices,
    int64_t                   inTick,
    int64_t                   outTick,
    float                     speed,
    bool                      muteAudio
) {
    float const clampedSpeed = state::editing::model::clampClipSpeed(speed);
    if (!slices.empty()) {
        auto& prev = slices.back();
        if (prev.outTick == inTick && prev.muteAudio == muteAudio
            && quantizeSpeedMilli(prev.speed) == quantizeSpeedMilli(clampedSpeed)) {
            prev.outTick = outTick;
            return;
        }
    }
    bool const hardCutBefore = !slices.empty() && inTick != slices.back().outTick;
    slices.push_back(ExportSlice{inTick, outTick, clampedSpeed, muteAudio, hardCutBefore});
}

} // namespace

std::filesystem::path buildExportOutputPath(ExportSettings const& settings) {
    auto const baseName = outputBaseName(settings.outputName);
    if (settings.format == ExportFormat::Mp4Video) return settings.outputDirectory / utf8Path(baseName + ".mp4");
    // A frame sequence is many files, so it still needs a folder of its own to live in.
    return settings.outputDirectory / utf8Path(baseName);
}

std::filesystem::path findAvailableExportPath(std::filesystem::path const& desired) {
    std::error_code error;
    if (!std::filesystem::exists(desired, error) && !error) return desired;

    auto const parent    = desired.parent_path();
    auto const extension = desired.extension();
    auto const stem      = desired.stem();
    for (int index = 1; index < 10'000; ++index) {
        auto suffix    = std::filesystem::path{" (" + std::to_string(index) + ")"};
        auto candidate = parent / (stem.native() + suffix.native() + extension.native());
        if (!std::filesystem::exists(candidate, error) || error) return candidate;
    }
    return desired;
}

std::optional<ExportFramePlan> CompiledExportPlan::frame(uint64_t frameIndex) const {
    if (frameIndex >= frameCount || segments.empty()) return std::nullopt;
    if (frameIndex > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;

    auto const it = std::upper_bound(
        segments.begin(),
        segments.end(),
        frameIndex,
        [](uint64_t target, CompiledExportSegment const& seg) { return target < seg.firstFrame; }
    );
    if (it == segments.begin()) return std::nullopt;
    auto const& seg = *std::prev(it);
    if (frameIndex >= seg.firstFrame + seg.frameCount) return std::nullopt;

    uint64_t const localIndex  = frameIndex - seg.firstFrame;
    int64_t        frameOffset = 0;
    if (!checkedSignedMultiply(static_cast<int64_t>(localIndex), seg.replayStepNumerator, frameOffset)) {
        return std::nullopt;
    }

    int64_t replayTickNumerator = 0;
    if (!checkedSignedAdd(seg.replayStartNumerator, frameOffset, replayTickNumerator)) return std::nullopt;
    replayTickNumerator = std::min(seg.maxNumerator, replayTickNumerator);

    int64_t ptsNumerator = 0;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameIndex), settings.frameRate.denominator, ptsNumerator)) {
        return std::nullopt;
    }

    ExportFramePlan result;
    result.replayTickNumerator   = replayTickNumerator;
    result.replayTickDenominator = seg.replayDenominator;
    result.ticket.frameIndex     = frameIndex;
    result.ticket.ptsNumerator   = ptsNumerator;
    result.ticket.ptsDenominator = settings.frameRate.numerator;
    result.hardCut               = (localIndex == 0 && seg.hardCutBefore);
    result.muteAudio             = seg.muteAudio;
    return result;
}

ExportPlanCompileResult
ExportPlanCompiler::compile(ExportSettings const& settings, state::editing::model::EditorStateExt const& project) {
    if (settings.format != ExportFormat::PngSequence && settings.format != ExportFormat::Mp4Video) {
        return failure(ExportError::InvalidSettings, "The requested export format is not supported");
    }
    if (settings.rangeMode != ExportRangeMode::EnabledClips && settings.rangeMode != ExportRangeMode::SequenceInOut
        && settings.rangeMode != ExportRangeMode::SingleClip) {
        return failure(ExportError::InvalidSettings, "The requested export range mode is not supported");
    }
    if (settings.outputDirectory.empty() || !validOutputName(settings.outputName)
        || outputBaseName(settings.outputName).empty()) {
        return failure(ExportError::InvalidOutputPath, "The export output path is invalid");
    }
    if (settings.frameRate.numerator <= 0 || settings.frameRate.denominator <= 0
        || settings.frameRate.numerator > MaxFrameRateValue || settings.frameRate.denominator > MaxFrameRateValue) {
        return failure(ExportError::InvalidSettings, "The export frame rate is invalid");
    }
    if ((settings.resolutionX == 0) != (settings.resolutionY == 0)) {
        return failure(ExportError::InvalidSettings, "The export resolution must specify both width and height");
    }
    if (settings.resolutionX == 0 && settings.ssaa != 1) {
        return failure(ExportError::InvalidSettings, "SSAA requires an explicit output resolution");
    }
    if (settings.resolutionX > MaxResolution || settings.resolutionY > MaxResolution || settings.ssaa == 0
        || settings.ssaa > MaxSsaa) {
        return failure(ExportError::InvalidSettings, "The export resolution or SSAA value is invalid");
    }
    if (settings.resolutionX != 0) {
        if (settings.resolutionX < 16 || settings.resolutionY < 16) {
            return failure(ExportError::InvalidSettings, "The export resolution is too small");
        }
        auto const scaledWidth  = static_cast<uint64_t>(settings.resolutionX) * settings.ssaa;
        auto const scaledHeight = static_cast<uint64_t>(settings.resolutionY) * settings.ssaa;
        if (scaledWidth > MaxResolution || scaledHeight > MaxResolution
            || static_cast<uint64_t>(settings.resolutionX) * settings.resolutionY > MaxFramePixels
            || scaledWidth * scaledHeight > MaxFramePixels) {
            return failure(ExportError::InvalidSettings, "The supersampled render resolution is too large");
        }
    }
    if (settings.warmupFrames > MaxWarmupFrames) {
        return failure(ExportError::InvalidSettings, "The export warm-up frame count is too large");
    }
    if (settings.extraRenders > MaxExportExtraRenders) {
        return failure(ExportError::InvalidSettings, "The export extra render count is too large");
    }
    if (settings.videoCrf > MaxExportCrf || settings.encodeSpeed > VideoEncodeSpeed::Fastest) {
        return failure(ExportError::InvalidSettings, "The video encoder quality or speed setting is invalid");
    }
    if (project.totalTicks < 0) {
        return failure(ExportError::InvalidTimeline, "The replay duration is invalid");
    }

    ExportSettings normalized = settings;
    normalized.frameRate      = normalizeFrameRate(normalized.frameRate);

    std::vector<ExportSlice> slices;
    switch (normalized.rangeMode) {
    case ExportRangeMode::EnabledClips: {
        auto const composited = state::editing::ClipOps::collectCompositedSlices(project, true);
        if (composited.empty()) {
            return failure(ExportError::InvalidTimeline, "No enabled clips are available to export");
        }
        for (auto const& slice : composited) {
            if (slice.inTick < 0 || slice.inTick >= slice.outTick || slice.outTick > project.totalTicks) {
                return failure(ExportError::InvalidTimeline, "An enabled clip is out of timeline bounds");
            }
            appendOrMergeSlice(slices, slice.inTick, slice.outTick, slice.speed, slice.muted);
        }
        break;
    }
    case ExportRangeMode::SequenceInOut: {
        if (settings.startTick < 0 || settings.endTick <= settings.startTick) {
            return failure(ExportError::InvalidTimeline, "The export timeline must have a positive duration");
        }
        if (settings.endTick > project.totalTicks) {
            return failure(ExportError::InvalidTimeline, "The export timeline exceeds the replay duration");
        }

        if (project.cameras.empty()) {
            slices.push_back(ExportSlice{settings.startTick, settings.endTick, 1.0f, false, false});
            break;
        }

        auto const composited = state::editing::ClipOps::collectCompositedSlices(project, true);
        for (auto const& slice : composited) {
            int64_t const cStart = std::max<int64_t>(slice.inTick, settings.startTick);
            int64_t const cEnd   = std::min<int64_t>(slice.outTick, settings.endTick);
            if (cStart >= cEnd) continue;
            appendOrMergeSlice(slices, cStart, cEnd, slice.speed, slice.muted);
        }
        if (slices.empty()) {
            return failure(ExportError::InvalidTimeline, "No enabled clips lie within the selected export range");
        }
        break;
    }
    case ExportRangeMode::SingleClip: {
        if (settings.selectedClipIds.size() != 1) {
            return failure(ExportError::InvalidTimeline, "Single-clip export requires exactly one selected clip");
        }
        auto const* clip = state::editing::ClipOps::findClipById(project, settings.selectedClipIds.front());
        if (!clip || clip->inTick < 0 || clip->inTick >= clip->outTick || clip->outTick > project.totalTicks) {
            return failure(ExportError::InvalidTimeline, "The selected clip is missing or out of timeline bounds");
        }
        slices.push_back(
            ExportSlice{
                clip->inTick,
                clip->outTick,
                state::editing::model::clampClipSpeed(clip->speed),
                clip->muted,
                false,
            }
        );
        break;
    }
    }

    if (slices.empty()) {
        return failure(ExportError::InvalidTimeline, "The export timeline produced no segments");
    }

    std::vector<CompiledExportSegment> segments;
    segments.reserve(slices.size());
    uint64_t totalFrames = 0;
    int64_t  maxOutTick  = 0;

    for (auto const& slice : slices) {
        int64_t const speedMilli =
            std::clamp<int64_t>(std::llround(static_cast<double>(slice.speed) * 1000.0), 50, 32000);
        int64_t const speedGcd = gcd(speedMilli, 1000);
        int64_t const speedNum = speedMilli / speedGcd;
        int64_t const speedDen = 1000 / speedGcd;

        int64_t stepRawNum = 0;
        int64_t stepRawDen = 0;
        int64_t tmpDen     = 0;
        if (!checkedSignedMultiply(ReplayTicksPerSecond, normalized.frameRate.denominator, tmpDen)
            || !checkedSignedMultiply(tmpDen, speedNum, stepRawNum)) {
            return failure(ExportError::InvalidSettings, "The export frame rate is too large");
        }
        if (!checkedSignedMultiply(normalized.frameRate.numerator, speedDen, stepRawDen)) {
            return failure(ExportError::InvalidSettings, "The export frame rate is too large");
        }

        int64_t const stepGcd   = gcd(stepRawNum, stepRawDen);
        int64_t const stepNum   = stepRawNum / stepGcd;
        int64_t const replayDen = stepRawDen / stepGcd;

        uint64_t const durationTicks = static_cast<uint64_t>(slice.outTick - slice.inTick);
        uint64_t const whole         = durationTicks / static_cast<uint64_t>(stepNum);
        uint64_t const rem           = durationTicks % static_cast<uint64_t>(stepNum);
        uint64_t       fracNumerator = 0;
        if (!checkedMultiply(rem, static_cast<uint64_t>(replayDen), fracNumerator)) {
            return failure(ExportError::InvalidTimeline, "The export duration is too large");
        }
        uint64_t const fracFrames =
            (fracNumerator + static_cast<uint64_t>(stepNum) - 1) / static_cast<uint64_t>(stepNum);

        uint64_t segFrameCount = 0;
        if (!checkedMultiply(whole, static_cast<uint64_t>(replayDen), segFrameCount)
            || !checkedAdd(segFrameCount, fracFrames, segFrameCount)) {
            return failure(ExportError::InvalidTimeline, "The export timeline produces too many frames");
        }
        if (segFrameCount == 0) segFrameCount = 1;

        uint64_t nextTotalFrames = 0;
        if (!checkedAdd(totalFrames, segFrameCount, nextTotalFrames) || nextTotalFrames > MaxExportFrames) {
            return failure(ExportError::InvalidTimeline, "The export timeline produces too many frames");
        }
        if (nextTotalFrames - 1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return failure(ExportError::InvalidTimeline, "The export frame index is too large");
        }

        int64_t startNum = 0;
        if (!checkedSignedMultiply(slice.inTick, replayDen, startNum)) {
            return failure(ExportError::InvalidTimeline, "The export start tick is too large");
        }
        int64_t maxNum = 0;
        if (!checkedSignedMultiply(slice.outTick, replayDen, maxNum)) {
            return failure(ExportError::InvalidTimeline, "The export end tick is too large");
        }
        int64_t lastOffset = 0;
        if (!checkedSignedMultiply(static_cast<int64_t>(segFrameCount - 1), stepNum, lastOffset)) {
            return failure(ExportError::InvalidTimeline, "The export timeline is too large");
        }
        int64_t lastSample = 0;
        if (!checkedSignedAdd(startNum, lastOffset, lastSample)) {
            return failure(ExportError::InvalidTimeline, "The export timeline is too large");
        }
        (void)lastSample;

        segments.push_back(
            CompiledExportSegment{
                slice.inTick,
                slice.outTick,
                totalFrames,
                segFrameCount,
                startNum,
                stepNum,
                replayDen,
                maxNum,
                slice.hardCutBefore,
                slice.muteAudio,
            }
        );
        totalFrames = nextTotalFrames;
        maxOutTick  = std::max(maxOutTick, slice.outTick);
    }

    if (totalFrames == 0 || totalFrames > MaxExportFrames) {
        return failure(ExportError::InvalidTimeline, "The export timeline produces too many frames");
    }

    CompiledExportPlan plan;
    plan.settings           = std::move(normalized);
    plan.settings.startTick = segments.front().inTick;
    plan.settings.endTick   = maxOutTick;
    plan.outputPath         = buildExportOutputPath(plan.settings);
    plan.frameCount         = totalFrames;
    plan.segments           = std::move(segments);
    if (plan.segments.size() == 1) {
        plan.replayTickStartNumerator = plan.segments[0].replayStartNumerator;
        plan.replayTickStepNumerator  = plan.segments[0].replayStepNumerator;
        plan.replayTickDenominator    = plan.segments[0].replayDenominator;
    }
    return ExportPlanCompileResult{std::move(plan), ExportError::None, {}};
}

} // namespace playback::exporting
