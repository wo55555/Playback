#pragma once

#include "playback/visuals/FrameCaptureTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace playback::state::editing::model {
struct EditorStateExt;
}

namespace playback::exporting {

struct FrameRate {
    int64_t numerator{60};
    int64_t denominator{1};
};

enum class ExportFormat : uint8_t { Mp4Video, PngSequence };

// Maps to the libx264 preset: faster presets trade file size and some sharpness for encode speed.
enum class VideoEncodeSpeed : uint8_t { Quality, Fast, Fastest };

[[nodiscard]] constexpr char const* x264Preset(VideoEncodeSpeed speed) {
    switch (speed) {
    case VideoEncodeSpeed::Fast:
        return "veryfast";
    case VideoEncodeSpeed::Fastest:
        return "ultrafast";
    case VideoEncodeSpeed::Quality:
    default:
        return "medium";
    }
}

constexpr uint32_t MaxExportCrf        = 51;
constexpr uint32_t MaxExportResolution = 16'384;
constexpr uint32_t MaxExportSsaa       = 4;
constexpr uint64_t MaxExportPixels     = (512ull * 1024 * 1024) / 4;
// Noise falls roughly with the square root of the pass count, so each doubling costs twice the render time.
constexpr uint32_t MaxExportConvergenceFrames = 1'000;

// The supersampled surface, not the output, is what the renderer and the capture path have to hold.
[[nodiscard]] constexpr bool supersampleFits(uint32_t width, uint32_t height, uint32_t ssaa) {
    if (ssaa == 0 || width == 0 || height == 0) return false;
    auto const scaledWidth  = static_cast<uint64_t>(width) * ssaa;
    auto const scaledHeight = static_cast<uint64_t>(height) * ssaa;
    return scaledWidth <= MaxExportResolution && scaledHeight <= MaxExportResolution
        && scaledWidth * scaledHeight <= MaxExportPixels;
}

struct ExportSettings {
    std::filesystem::path outputDirectory{"mods/playback/exports"};
    std::string           outputName{"replay-export"};
    int64_t               startTick{};
    int64_t               endTick{};
    FrameRate             frameRate{};
    ExportFormat          format{ExportFormat::PngSequence};

    uint32_t resolutionX{};
    uint32_t resolutionY{};
    uint32_t ssaa{1};
    uint32_t warmupFrames{60};
    // Extra renders per frame so ray-traced temporal accumulation can settle; each one costs a full render.
    uint32_t convergenceFrames{0};
    // Experimental: hand out client ticks as soon as the export asks instead of waiting for the 20 Hz game timer.
    bool accelerateClient{false};
    // Whose eyes to use while a first-person timeline segment is active. Empty selects the recorded local player.
    // The timeline decides when; this is who. One subject covers the whole export.
    std::string firstPersonSubject;
    // MP4 only; the defaults are what the writer used before these were configurable.
    VideoEncodeSpeed encodeSpeed{VideoEncodeSpeed::Quality};
    uint32_t         videoCrf{18};
};

enum class ExportError : uint8_t {
    None,
    InvalidSettings,
    InvalidTimeline,
    InvalidOutputPath,
    OutputExists,
    DirectoryCreateFailed,
    WriterUnavailable,
    ReplayUnavailable,
    CaptureUnavailable,
    CaptureFailed,
    InvalidFrame,
    FrameOutOfOrder,
    FrameCountMismatch,
    WriteFailed,
    Cancelled,
};

enum class ExportState : uint8_t {
    Idle,
    Preparing,
    Running,
    Finalizing,
    Cancelling,
    Completed,
    Cancelled,
    Faulted,
};

[[nodiscard]] constexpr bool isExportActive(ExportState state) noexcept {
    return state == ExportState::Preparing || state == ExportState::Running || state == ExportState::Finalizing
        || state == ExportState::Cancelling;
}

struct ExportStatus {
    ExportState           state{ExportState::Idle};
    ExportError           error{ExportError::None};
    ExportFormat          format{ExportFormat::PngSequence};
    std::string           message;
    std::filesystem::path outputPath;
    std::filesystem::path latestFramePath;
    uint64_t              submittedFrames{};
    uint64_t              writtenFrames{};
    uint64_t              totalFrames{};
};

struct ExportFramePlan {
    int64_t              replayTickNumerator{};
    int64_t              replayTickDenominator{1};
    visuals::FrameTicket ticket;
};

struct CompiledExportPlan {
    ExportSettings        settings;
    std::filesystem::path outputPath;
    uint64_t              frameCount{};
    int64_t               replayTickStartNumerator{};
    int64_t               replayTickStepNumerator{};
    int64_t               replayTickDenominator{1};

    [[nodiscard]] std::optional<ExportFramePlan> frame(uint64_t frameIndex) const;
};

struct ExportPlanCompileResult {
    std::optional<CompiledExportPlan> plan;
    ExportError                       error{ExportError::None};
    std::string                       message;

    [[nodiscard]] explicit operator bool() const { return plan.has_value(); }
};

} // namespace playback::exporting
