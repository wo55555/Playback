#pragma once

#include "playback/state/editing/models/EditorStateExt.h"

#include <string>

namespace playback::state::editing {

constexpr int kProjectFormatVersion = 1;

[[nodiscard]] std::string serializeProject(model::EditorStateExt const& project);

// Returns false and fills `error` when the payload is not a project of a supported version.
[[nodiscard]] bool deserializeProject(std::string const& payload, model::EditorStateExt& project, std::string& error);

} // namespace playback::state::editing
