#include "PathUtils.h"

#include "playback/Playback.h"

#include <filesystem>
#include <system_error>

namespace playback::utils {

PathUtils::PathUtils() : mDataDir(Playback::getInstance().getSelf().getDataDir()) {}

std::filesystem::path PathUtils::getReplaysDir() { return getInstance().mDataDir / "replays"; }

std::filesystem::path PathUtils::getSharedTempDir() { return getInstance().mDataDir / "temp"; }

std::filesystem::path PathUtils::createTemp(std::string_view uuid) {
    std::error_code       ec;
    std::filesystem::path path = getTempPath(uuid);
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            throw std::runtime_error("Failed to check existence: " + ec.message());
        }

        if (!std::filesystem::create_directories(path, ec) || ec) {
            throw std::runtime_error("Failed to create dir: " + ec.message());
        }
    }

    return path;
}

std::filesystem::path PathUtils::getTempPath(std::string_view uuid) { return getSharedTempDir() / uuid; }

} // namespace playback::utils
