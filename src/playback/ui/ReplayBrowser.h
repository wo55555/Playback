#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback::ui {

enum class ReplaySort {
    LastModified,
    ReplayName,
    WorldName,
    Duration,
    FileSize,
};

struct ReplaySummary {
    std::filesystem::path            path;
    std::string                      replayId;
    std::string                      replayName;
    std::string                      worldName;
    int                              durationTicks = 0;
    int                              totalTicks    = 0;
    std::uintmax_t                   fileSize      = 0;
    std::filesystem::file_time_type  lastModified{};
    bool                             canOpen = false;
    std::string                      problem;

    [[nodiscard]] std::string displayName() const;
    [[nodiscard]] bool        matches(std::string_view filter) const;
};

class ReplayBrowser {
public:
    [[nodiscard]] static std::vector<ReplaySummary> loadReplays();
    [[nodiscard]] static std::vector<ReplaySummary> loadReplays(std::filesystem::path const& replayDir);

    static void sortReplays(
        std::vector<ReplaySummary>& replays,
        ReplaySort                  sort       = ReplaySort::LastModified,
        bool                        descending = true
    );

    [[nodiscard]] static std::vector<ReplaySummary> filterReplays(
        std::vector<ReplaySummary> const& replays,
        std::string_view                  filter
    );

    [[nodiscard]] static std::optional<ReplaySummary> findReplay(std::string_view replayIdOrPath);

    static bool openReplay(ReplaySummary const& replay);
    static bool openReplay(std::filesystem::path const& replayPath);
};

} // namespace playback::ui
