#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state {

enum class ReplayBrowserOperation {
    None,
    Refreshing,
    OpeningReplay,
    ImportingReplay,
    DeletingReplay,
    RenamingReplay,
    ShowingInFolder,
};

struct ReplayBrowserEntry {
    std::filesystem::path           path;
    std::string                     replayId;
    std::string                     replayName;
    std::string                     worldName;
    int                             durationTicks{};
    int                             totalTicks{};
    std::uintmax_t                  fileSize{};
    std::filesystem::file_time_type lastModified{};
    bool                            canOpen{};
    std::string                     problem;
    std::string                     thumbnailPng;

    [[nodiscard]] std::string displayName() const {
        return replayName.empty() || replayName == "Unnamed" ? path.stem().string() : replayName;
    }

    [[nodiscard]] bool matches(std::string_view filter) const {
        auto lower = [](std::string_view value) {
            std::string result(value);
            for (char& ch : result) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return result;
        };
        auto const needle = lower(filter);
        return needle.empty() || lower(displayName()).find(needle) != std::string::npos
            || lower(replayId).find(needle) != std::string::npos || lower(worldName).find(needle) != std::string::npos;
    }
};

struct ReplayBrowserSnapshot {
    std::uint64_t                   revision{};
    std::vector<ReplayBrowserEntry> replays;
};

struct ReplayBrowserState {
    bool                                         visible{};
    ReplayBrowserOperation                       operation{ReplayBrowserOperation::None};
    std::string                                  error;
    std::shared_ptr<ReplayBrowserSnapshot const> snapshot;

    [[nodiscard]] bool busy() const { return operation != ReplayBrowserOperation::None; }
};

} // namespace playback::state
