#pragma once

#include "ExportTypes.h"

namespace playback::exporting {

class ExportPlanCompiler {
public:
    [[nodiscard]] static ExportPlanCompileResult
    compile(ExportSettings const& settings, state::editing::model::EditorStateExt const& project);
};

[[nodiscard]] std::filesystem::path buildExportOutputPath(ExportSettings const& settings);

} // namespace playback::exporting
