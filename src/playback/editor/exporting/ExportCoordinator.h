#pragma once

#include "IFrameWriter.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace playback::editor::exporting {

class ExportCoordinator {
public:
    using WriterFactory = std::function<std::unique_ptr<IFrameWriter>(ExportFormat)>;

    ExportCoordinator();
    explicit ExportCoordinator(WriterFactory factory);
    ~ExportCoordinator();

    ExportCoordinator(ExportCoordinator const&)            = delete;
    ExportCoordinator& operator=(ExportCoordinator const&) = delete;

    [[nodiscard]] bool                    start(ExportSettings settings, editing::model::EditorStateExt const& project);
    [[nodiscard]] FrameWriterSubmitResult trySubmit(visuals::CapturedFrame& frame);
    [[nodiscard]] bool                    finish(); // Requests asynchronous finalization.
    void                                  fail(ExportError error, std::string message);
    void                                  cancel();
    void                                  reset();

    [[nodiscard]] ExportStatus                      status() const;
    [[nodiscard]] std::optional<CompiledExportPlan> plan() const;
    [[nodiscard]] static bool                       isFormatAvailable(ExportFormat format);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace playback::editor::exporting
