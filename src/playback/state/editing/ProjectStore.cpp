#include "playback/state/editing/ProjectStore.h"

#include "playback/Playback.h"
#include "playback/state/editing/ProjectSerializer.h"
#include "playback/utils/PathUtils.h"

#include <fstream>
#include <system_error>
#include <utility>

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

std::filesystem::path ProjectStore::backupPathFor(std::filesystem::path const& path) {
    if (path.empty()) return {};
    return std::filesystem::path(path).concat(".old");
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

    if (exists(path)) {
        auto const      backup = backupPathFor(path);
        std::error_code backupEc;
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, backupEc);
        if (backupEc) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Unable to update backup project {}: {}",
                backup.string(),
                backupEc.message()
            );
        }
    }

    ec.clear();
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code removeEc;
        std::filesystem::remove(temporary, removeEc);
        error = ec.message();
        return false;
    }
    return true;
}

bool ProjectStore::load(std::filesystem::path const& path, model::EditorStateExt& project, std::string& error) {
    auto const loadFile = [](std::filesystem::path const& filePath, model::EditorStateExt& out, std::string& outError) {
        std::ifstream input(filePath, std::ios::binary);
        if (!input) {
            outError = "unable to open " + filePath.string();
            return false;
        }
        std::string const payload{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        return deserializeProject(payload, out, outError);
    };

    model::EditorStateExt loaded;
    std::string           primaryError;
    if (loadFile(path, loaded, primaryError)) {
        project = std::move(loaded);
        error.clear();
        return true;
    }

    if (exists(path)) {
        auto const backup = backupPathFor(path);
        if (exists(backup)) {
            model::EditorStateExt recovered;
            std::string           backupError;
            if (loadFile(backup, recovered, backupError)) {
                project = std::move(recovered);
                error.clear();
                Playback::getInstance().getSelf().getLogger().warn(
                    "Primary project {} failed to load ({}); recovered from backup {}",
                    path.string(),
                    primaryError,
                    backup.string()
                );
                return true;
            }
        }
    }

    error = std::move(primaryError);
    return false;
}

bool ProjectStore::exists(std::filesystem::path const& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace playback::state::editing
