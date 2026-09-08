#include "playback/state/editing/ProjectStore.h"

#include "playback/state/editing/ProjectSerializer.h"
#include "playback/utils/PathUtils.h"

#include <fstream>
#include <system_error>

namespace playback::state::editing {

namespace {

constexpr char kProjectExtension[] = ".pbproj";

std::string sanitizeStem(std::string_view replayPath) {
    auto stem = std::filesystem::path(replayPath).stem().string();
    if (stem.empty()) stem = "project";
    for (auto& ch : stem) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>'
            || ch == '|') {
            ch = '_';
        }
    }
    return stem;
}

} // namespace

std::filesystem::path ProjectStore::defaultProjectPath(std::string_view replayPath) {
    if (replayPath.empty()) return {};
    auto const path = utils::PathUtils::getProjectsDir() / (sanitizeStem(replayPath) + kProjectExtension);
    return path.lexically_normal();
}

bool ProjectStore::save(model::EditorStateExt const& project, std::filesystem::path const& path, std::string& error) {
    if (path.empty()) {
        error = "empty project path";
        return false;
    }

    std::error_code ec;
    auto const      parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec && !std::filesystem::is_directory(parent)) {
            error = ec.message();
            return false;
        }
    }

    auto const temporary = std::filesystem::path(path).concat(".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "unable to open " + temporary.string();
            return false;
        }
        output << serializeProject(project);
        if (!output) {
            error = "write failed for " + temporary.string();
            return false;
        }
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary);
        error = ec.message();
        return false;
    }
    return true;
}

bool ProjectStore::load(std::filesystem::path const& path, model::EditorStateExt& project, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open " + path.string();
        return false;
    }
    std::string const payload{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return deserializeProject(payload, project, error);
}

bool ProjectStore::exists(std::filesystem::path const& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace playback::state::editing
