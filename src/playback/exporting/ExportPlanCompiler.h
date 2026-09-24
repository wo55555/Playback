#pragma once

#include "ExportTypes.h"

namespace playback::exporting {

class ExportPlanCompiler {
public:
    [[nodiscard]] static ExportPlanCompileResult
    compile(ExportSettings const& settings, state::editing::model::EditorStateExt const& project);
};

[[nodiscard]] std::filesystem::path buildExportOutputPath(ExportSettings const& settings);

// Touches the filesystem, so it belongs at export start rather than in the per-frame dialog preview.
[[nodiscard]] std::filesystem::path findAvailableExportPath(std::filesystem::path const& desired);

} // namespace playback::exporting
