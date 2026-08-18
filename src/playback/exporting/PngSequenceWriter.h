#pragma once

#include "IFrameWriter.h"

#include <memory>

namespace playback::exporting {

class PngSequenceWriter final : public IFrameWriter {
public:
    explicit PngSequenceWriter(uint32_t capacity = 8);
    ~PngSequenceWriter() override;

    PngSequenceWriter(PngSequenceWriter const&)            = delete;
    PngSequenceWriter& operator=(PngSequenceWriter const&) = delete;

    [[nodiscard]] bool                    open(CompiledExportPlan const& plan) override;
    [[nodiscard]] FrameWriterSubmitResult trySubmit(visuals::CapturedFrame& frame) override;
    [[nodiscard]] bool                    requestFinish() override;
    void                                  requestCancel() override;
    void                                  wait() override;
    [[nodiscard]] FrameWriterStatus       status() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::exporting
