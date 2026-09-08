#pragma once

#include "playback/state/editing/models/EditorStateExt.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace playback::state::editing {

class ProjectStore {
public:
    // Default project file for a replay, used for autosave and implicit load.
    [[nodiscard]] static std::filesystem::path defaultProjectPath(std::string_view replayPath);

    [[nodiscard]] static bool
    save(model::EditorStateExt const& project, std::filesystem::path const& path, std::string& error);

    [[nodiscard]] static bool
    load(std::filesystem::path const& path, model::EditorStateExt& project, std::string& error);

    [[nodiscard]] static bool exists(std::filesystem::path const& path);
};

} // namespace playback::state::editing
