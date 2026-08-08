#include "ExportPlanCompiler.h"

#include "playback/editor/editing/models/EditorStateExt.h"

#include <cctype>
#include <limits>

namespace playback::editor::exporting {

namespace {

constexpr int64_t  ReplayTicksPerSecond = 20;
constexpr int64_t  MaxFrameRateValue    = 1'000'000;
constexpr uint64_t MaxExportFrames      = 1'000'000;

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

[[nodiscard]] std::string frameRateLabel(FrameRate rate) {
    rate              = normalizeFrameRate(rate);
    std::string label = std::to_string(rate.numerator);
    if (rate.denominator != 1) {
        label += '-';
        label += std::to_string(rate.denominator);
    }
    label += "fps";
    return label;
}

[[nodiscard]] std::filesystem::path utf8Path(std::string const& value) {
    auto const* begin = reinterpret_cast<char8_t const*>(value.data());
    return std::filesystem::path(std::u8string{begin, begin + value.size()});
}

[[nodiscard]] ExportPlanCompileResult failure(ExportError error, char const* message) {
    return ExportPlanCompileResult{std::nullopt, error, message};
}

} // namespace

std::filesystem::path buildExportOutputPath(ExportSettings const& settings) {
    auto const baseName = outputBaseName(settings.outputName);
    auto const variant  = baseName + "_t" + std::to_string(settings.startTick) + "-" + std::to_string(settings.endTick)
                       + "_" + frameRateLabel(settings.frameRate);
    auto const root = settings.outputDirectory / utf8Path(baseName);

    if (settings.format == ExportFormat::Mp4Video) return root / utf8Path(variant + ".mp4");
    return root / utf8Path(variant + "_frames");
}

std::optional<ExportFramePlan> CompiledExportPlan::frame(uint64_t frameIndex) const {
    if (frameIndex >= frameCount) return std::nullopt;

    int64_t frameOffset = 0;
    if (frameIndex > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameIndex), replayTickStepNumerator, frameOffset)) {
        return std::nullopt;
    }

    int64_t replayTickNumerator = 0;
    if (!checkedSignedAdd(replayTickStartNumerator, frameOffset, replayTickNumerator)) return std::nullopt;

    int64_t ptsNumerator = 0;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameIndex), settings.frameRate.denominator, ptsNumerator)) {
        return std::nullopt;
    }

    ExportFramePlan result;
    result.replayTickNumerator   = replayTickNumerator;
    result.replayTickDenominator = replayTickDenominator;
    result.ticket.frameIndex     = frameIndex;
    result.ticket.ptsNumerator   = ptsNumerator;
    result.ticket.ptsDenominator = settings.frameRate.numerator;
    return result;
}

ExportPlanCompileResult
ExportPlanCompiler::compile(ExportSettings const& settings, editing::model::EditorStateExt const& project) {
    if (settings.format != ExportFormat::PngSequence && settings.format != ExportFormat::Mp4Video) {
        return failure(ExportError::InvalidSettings, "The requested export format is not supported");
    }
    if (settings.outputDirectory.empty() || !validOutputName(settings.outputName)
        || outputBaseName(settings.outputName).empty()) {
        return failure(ExportError::InvalidOutputPath, "The export output path is invalid");
    }
    if (settings.startTick < 0 || settings.endTick <= settings.startTick) {
        return failure(ExportError::InvalidTimeline, "The export timeline must have a positive duration");
    }
    if (project.totalTicks < 0 || settings.endTick > project.totalTicks) {
        return failure(ExportError::InvalidTimeline, "The export timeline exceeds the replay duration");
    }
    if (settings.frameRate.numerator <= 0 || settings.frameRate.denominator <= 0
        || settings.frameRate.numerator > MaxFrameRateValue || settings.frameRate.denominator > MaxFrameRateValue) {
        return failure(ExportError::InvalidSettings, "The export frame rate is invalid");
    }

    ExportSettings normalized = settings;
    normalized.frameRate      = normalizeFrameRate(normalized.frameRate);

    uint64_t const durationTicks = static_cast<uint64_t>(settings.endTick - settings.startTick);
    uint64_t       denominator   = 0;
    if (!checkedMultiply(
            static_cast<uint64_t>(ReplayTicksPerSecond),
            static_cast<uint64_t>(normalized.frameRate.denominator),
            denominator
        )) {
        return failure(ExportError::InvalidSettings, "The export frame rate denominator is too large");
    }

    uint64_t wholeFrames         = durationTicks / denominator;
    uint64_t remainder           = durationTicks % denominator;
    uint64_t fractionalNumerator = 0;
    if (!checkedMultiply(remainder, static_cast<uint64_t>(normalized.frameRate.numerator), fractionalNumerator)) {
        return failure(ExportError::InvalidTimeline, "The export duration is too large");
    }
    uint64_t fractionalFrames = fractionalNumerator / denominator;
    if (fractionalNumerator % denominator != 0) ++fractionalFrames;

    uint64_t frameCount = 0;
    if (!checkedMultiply(wholeFrames, static_cast<uint64_t>(normalized.frameRate.numerator), frameCount)
        || !checkedAdd(frameCount, fractionalFrames, frameCount) || frameCount == 0 || frameCount > MaxExportFrames) {
        return failure(ExportError::InvalidTimeline, "The export timeline produces too many frames");
    }
    if (frameCount - 1 > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return failure(ExportError::InvalidTimeline, "The export frame index is too large");
    }

    int64_t replayStartNumerator = 0;
    if (!checkedSignedMultiply(settings.startTick, normalized.frameRate.numerator, replayStartNumerator)) {
        return failure(ExportError::InvalidTimeline, "The export start tick is too large");
    }
    int64_t replayStepNumerator = 0;
    if (!checkedSignedMultiply(ReplayTicksPerSecond, normalized.frameRate.denominator, replayStepNumerator)) {
        return failure(ExportError::InvalidSettings, "The export frame rate is too large");
    }
    int64_t lastOffset = 0;
    if (!checkedSignedMultiply(static_cast<int64_t>(frameCount - 1), replayStepNumerator, lastOffset)) {
        return failure(ExportError::InvalidTimeline, "The export timeline is too large");
    }
    int64_t lastSample = 0;
    if (!checkedSignedAdd(replayStartNumerator, lastOffset, lastSample)) {
        return failure(ExportError::InvalidTimeline, "The export timeline is too large");
    }
    (void)lastSample;

    CompiledExportPlan plan;
    plan.settings                 = std::move(normalized);
    plan.outputPath               = buildExportOutputPath(plan.settings);
    plan.frameCount               = frameCount;
    plan.replayTickStartNumerator = replayStartNumerator;
    plan.replayTickStepNumerator  = replayStepNumerator;
    plan.replayTickDenominator    = plan.settings.frameRate.numerator;
    return ExportPlanCompileResult{std::move(plan), ExportError::None, {}};
}

} // namespace playback::editor::exporting
