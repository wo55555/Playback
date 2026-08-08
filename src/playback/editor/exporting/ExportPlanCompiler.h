#pragma once

#include "ExportTypes.h"

namespace playback::editor::exporting {

class ExportPlanCompiler {
public:
    [[nodiscard]] static ExportPlanCompileResult
    compile(ExportSettings const& settings, editing::model::EditorStateExt const& project);
};

[[nodiscard]] std::filesystem::path buildExportOutputPath(ExportSettings const& settings);

} // namespace playback::editor::exporting
