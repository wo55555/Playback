#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace playback::utils {

class PathUtils {
private:
    std::filesystem::path mDataDir;

private:
    PathUtils();

public:
    [[nodiscard]] static std::filesystem::path getReplaysDir();

    [[nodiscard]] static std::filesystem::path getSharedTempDir();

    [[nodiscard]] static std::filesystem::path createTemp(std::string_view uuid);

    [[nodiscard]] static std::filesystem::path getTempPath(std::string_view uuid);

    static void deleteTemp(std::string uuid);

public:
    [[nodiscard]] static PathUtils& getInstance() {
        static PathUtils instance;
        return instance;
    }
};

} // namespace playback::utils
