#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::io {

enum class ReplaySort {
    LastModified,
    ReplayName,
    WorldName,
    Duration,
    FileSize,
};

struct ReplaySummary {
    std::filesystem::path           path;
    std::string                     replayId;
    std::string                     replayName;
    std::string                     worldName;
    int                             durationTicks = 0;
    int                             totalTicks    = 0;
    std::uintmax_t                  fileSize      = 0;
    std::filesystem::file_time_type lastModified{};
    bool                            canOpen = false;
    std::string                     problem;
    std::string                     thumbnailPng;

    [[nodiscard]] std::string displayName() const;
    [[nodiscard]] bool        matches(std::string_view filter) const;
};

class ReplayLibrary {
public:
    [[nodiscard]] static std::vector<ReplaySummary> loadReplays();
    [[nodiscard]] static std::vector<ReplaySummary> loadReplays(std::filesystem::path const& replayDir);

    static void sortReplays(
        std::vector<ReplaySummary>& replays,
        ReplaySort                  sort       = ReplaySort::LastModified,
        bool                        descending = true
    );

    [[nodiscard]] static std::vector<ReplaySummary>
    filterReplays(std::vector<ReplaySummary> const& replays, std::string_view filter);

    [[nodiscard]] static std::optional<ReplaySummary> findReplay(std::string_view replayIdOrPath);

    [[nodiscard]] static bool importReplay(std::filesystem::path const& source, std::string& error);
    [[nodiscard]] static bool deleteReplay(ReplaySummary const& replay, std::string& error);
    [[nodiscard]] static bool showInFolder(ReplaySummary const& replay);

    // 鍚屾椂淇敼鍥炴斁鍏冩暟鎹悕绉颁笌鏂囦欢鏈韩鍚嶇О锛涙柊鍚嶄細鑷姩鍘绘帀闈炴硶瀛楃骞惰ˉ鍏?.playback
    // 鎵╁睍鍚嶃€?
    [[nodiscard]] static bool renameReplay(ReplaySummary const& replay, std::string_view newName, std::string& error);
};

} // namespace playback::io
