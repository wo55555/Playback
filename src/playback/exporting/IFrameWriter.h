#pragma once

#include "ExportTypes.h"

#include <cstdint>
#include <string>

namespace playback::exporting {

enum class FrameWriterSubmitResult : uint8_t { Accepted, Backpressured, Closed, Failed };

enum class FrameWriterState : uint8_t { Idle, Running, Finishing, Cancelling, Completed, Cancelled, Faulted };

struct FrameWriterStatus {
    FrameWriterState      state{FrameWriterState::Idle};
    uint64_t              submittedFrames{};
    uint64_t              writtenFrames{};
    ExportError           error{ExportError::None};
    std::string           message;
    std::filesystem::path latestFramePath;
};

class IFrameWriter {
public:
    virtual ~IFrameWriter() = default;

    [[nodiscard]] virtual bool                    open(CompiledExportPlan const& plan)               = 0;
    [[nodiscard]] virtual FrameWriterSubmitResult trySubmit(visuals::CapturedFrame& frame) = 0;
    [[nodiscard]] virtual bool                    requestFinish()                                    = 0;
    virtual void                                  requestCancel()                                    = 0;
    virtual void                                  wait()                                             = 0;

    [[nodiscard]] virtual FrameWriterStatus status() const = 0;
};

} // namespace playback::exporting
