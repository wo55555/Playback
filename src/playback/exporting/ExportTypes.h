#pragma once

#include "playback/visuals/FrameTap.h"

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
    int64_t                        replayTickNumerator{};
    int64_t                        replayTickDenominator{1};
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
