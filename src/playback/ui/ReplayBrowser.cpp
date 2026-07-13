#include "ReplayBrowser.h"

#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/utils/PathUtils.h"

#include "zip.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace playback::ui {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

std::string lowerCopy(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool hasReplayExtension(std::filesystem::path const& path) {
    auto extension = lowerCopy(path.extension().string());
    return extension == ".playback" || extension == ".zip";
}

std::optional<std::string> readZipEntry(std::filesystem::path const& archivePath, std::string const& entryName) {
    auto archivePathString = archivePath.string();
    int  zipError          = 0;
    auto archive           = zip_open(archivePathString.c_str(), ZIP_RDONLY, &zipError);
    if (archive == nullptr) {
        zip_error_t error;
        zip_error_init_with_code(&error, zipError);
        getLogger().warn("Unable to open replay archive {}: {}", archivePath, zip_error_strerror(&error));
        zip_error_fini(&error);
        return std::nullopt;
    }

    zip_stat_t stat;
    zip_stat_init(&stat);
    if (zip_stat(archive, entryName.c_str(), 0, &stat) != 0) {
        getLogger().warn("Replay archive {} does not contain {}", archivePath, entryName);
        zip_close(archive);
        return std::nullopt;
    }

    auto* file = zip_fopen(archive, entryName.c_str(), 0);
    if (file == nullptr) {
        getLogger().warn("Unable to read {} from replay archive {}: {}", entryName, archivePath, zip_strerror(archive));
        zip_close(archive);
        return std::nullopt;
    }

    std::string data(static_cast<size_t>(stat.size), '\0');
    auto        readBytes = zip_fread(file, data.data(), data.size());
    zip_fclose(file);
    zip_close(archive);

    if (readBytes < 0 || static_cast<zip_uint64_t>(readBytes) != stat.size) {
        getLogger().warn("Unable to fully read {} from replay archive {}", entryName, archivePath);
        return std::nullopt;
    }

    return data;
}

ReplaySummary readReplaySummary(std::filesystem::directory_entry const& entry) {
    ReplaySummary summary;
    summary.path     = entry.path();
    summary.replayId = entry.path().filename().string();

    std::error_code ec;
    summary.fileSize = entry.file_size(ec);
    if (ec) {
        summary.fileSize = 0;
        ec.clear();
    }

    summary.lastModified = entry.last_write_time(ec);
    if (ec) {
        summary.lastModified = {};
        ec.clear();
    }

    auto metadata = readZipEntry(summary.path, "metadata.json");
    if (!metadata.has_value()) {
        summary.replayName = summary.replayId;
        summary.problem    = "missing metadata.json";
        return summary;
    }

    try {
        auto meta             = playback::functions::PlaybackMeta::fromJson(*metadata);
        summary.replayName   = meta.name.empty() ? summary.replayId : std::move(meta.name);
        summary.worldName    = std::move(meta.worldName);
        summary.durationTicks = meta.duration;
        summary.totalTicks    = meta.totalTicks;
        summary.canOpen       = true;
    } catch (std::exception const& e) {
        summary.replayName = summary.replayId;
        summary.problem    = e.what();
    }

    return summary;
}

int compareText(std::string const& left, std::string const& right) {
    auto normalizedLeft  = lowerCopy(left);
    auto normalizedRight = lowerCopy(right);
    if (normalizedLeft < normalizedRight) return -1;
    if (normalizedLeft > normalizedRight) return 1;
    return 0;
}

int replayDurationTicks(ReplaySummary const& replay) {
    return replay.totalTicks > 0 ? replay.totalTicks : replay.durationTicks;
}

template <typename Compare>
void sortWithDirection(std::vector<ReplaySummary>& replays, Compare compare, bool descending) {
    std::stable_sort(replays.begin(), replays.end(), [&](ReplaySummary const& left, ReplaySummary const& right) {
        if (descending) {
            return compare(right, left);
        }
        return compare(left, right);
    });
}

} // namespace

std::string ReplaySummary::displayName() const { return replayName.empty() ? replayId : replayName; }

bool ReplaySummary::matches(std::string_view filter) const {
    auto needle = lowerCopy(filter);
    if (needle.empty()) return true;

    return lowerCopy(displayName()).find(needle) != std::string::npos
        || lowerCopy(replayId).find(needle) != std::string::npos
        || lowerCopy(worldName).find(needle) != std::string::npos;
}

std::vector<ReplaySummary> ReplayBrowser::loadReplays() { return loadReplays(utils::PathUtils::getReplaysDir()); }

std::vector<ReplaySummary> ReplayBrowser::loadReplays(std::filesystem::path const& replayDir) {
    std::vector<ReplaySummary> replays;

    std::error_code ec;
    if (!std::filesystem::exists(replayDir, ec) || !std::filesystem::is_directory(replayDir, ec)) {
        return replays;
    }

    std::filesystem::directory_iterator iter(replayDir, ec);
    std::filesystem::directory_iterator end;
    while (!ec && iter != end) {
        auto const& entry = *iter;
        if (entry.is_regular_file(ec) && !ec && hasReplayExtension(entry.path())) {
            replays.emplace_back(readReplaySummary(entry));
        }
        iter.increment(ec);
    }

    if (ec) {
        getLogger().warn("Unable to enumerate replay folder {}: {}", replayDir, ec.message());
    }

    sortReplays(replays);
    return replays;
}

void ReplayBrowser::sortReplays(std::vector<ReplaySummary>& replays, ReplaySort sort, bool descending) {
    switch (sort) {
    case ReplaySort::ReplayName:
        sortWithDirection(
            replays,
            [](ReplaySummary const& left, ReplaySummary const& right) {
                auto result = compareText(left.displayName(), right.displayName());
                if (result != 0) return result < 0;
                return compareText(left.replayId, right.replayId) < 0;
            },
            descending
        );
        break;
    case ReplaySort::WorldName:
        sortWithDirection(
            replays,
            [](ReplaySummary const& left, ReplaySummary const& right) {
                auto result = compareText(left.worldName, right.worldName);
                if (result != 0) return result < 0;
                return compareText(left.replayId, right.replayId) < 0;
            },
            descending
        );
        break;
    case ReplaySort::Duration:
        sortWithDirection(
            replays,
            [](ReplaySummary const& left, ReplaySummary const& right) {
                auto const leftTicks  = replayDurationTicks(left);
                auto const rightTicks = replayDurationTicks(right);
                if (leftTicks != rightTicks) return leftTicks < rightTicks;
                return compareText(left.replayId, right.replayId) < 0;
            },
            descending
        );
        break;
    case ReplaySort::FileSize:
        sortWithDirection(
            replays,
            [](ReplaySummary const& left, ReplaySummary const& right) {
                if (left.fileSize != right.fileSize) return left.fileSize < right.fileSize;
                return compareText(left.replayId, right.replayId) < 0;
            },
            descending
        );
        break;
    case ReplaySort::LastModified:
    default:
        sortWithDirection(
            replays,
            [](ReplaySummary const& left, ReplaySummary const& right) {
                if (left.lastModified != right.lastModified) return left.lastModified < right.lastModified;
                return compareText(left.replayId, right.replayId) < 0;
            },
            descending
        );
        break;
    }
}

std::vector<ReplaySummary>
ReplayBrowser::filterReplays(std::vector<ReplaySummary> const& replays, std::string_view filter) {
    std::vector<ReplaySummary> filtered;
    std::copy_if(replays.begin(), replays.end(), std::back_inserter(filtered), [filter](ReplaySummary const& replay) {
        return replay.matches(filter);
    });
    return filtered;
}

std::optional<ReplaySummary> ReplayBrowser::findReplay(std::string_view replayIdOrPath) {
    std::filesystem::path requestedPath{std::string(replayIdOrPath)};

    std::error_code ec;
    if (std::filesystem::exists(requestedPath, ec) && std::filesystem::is_regular_file(requestedPath, ec)
        && hasReplayExtension(requestedPath)) {
        return readReplaySummary(std::filesystem::directory_entry(requestedPath));
    }

    auto replays = loadReplays();
    auto query   = lowerCopy(replayIdOrPath);
    for (auto const& replay : replays) {
        if (lowerCopy(replay.replayId) == query || lowerCopy(replay.path.stem().string()) == query) {
            return replay;
        }
    }

    auto replayDir = utils::PathUtils::getReplaysDir();
    for (auto const& extension : {".playback", ".zip"}) {
        auto candidate = replayDir / (std::string(replayIdOrPath) + extension);
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
            return readReplaySummary(std::filesystem::directory_entry(candidate));
        }
    }

    return std::nullopt;
}

bool ReplayBrowser::openReplay(ReplaySummary const& replay) {
    if (!replay.canOpen) {
        getLogger().error("Replay {} cannot be opened: {}", replay.path, replay.problem);
        return false;
    }
    return openReplay(replay.path);
}

bool ReplayBrowser::openReplay(std::filesystem::path const& replayPath) {
    std::error_code ec;
    if (!std::filesystem::exists(replayPath, ec) || !std::filesystem::is_regular_file(replayPath, ec)) {
        getLogger().error("Replay file does not exist: {}", replayPath);
        return false;
    }

    if (!hasReplayExtension(replayPath)) {
        getLogger().error("Unsupported replay file extension: {}", replayPath);
        return false;
    }

    if (!functions::ReplaySession::getInstance().start(replayPath)) {
        getLogger().error("Failed to start replay session from {}", replayPath);
        return false;
    }

    getLogger().info("Started replay session from {}", replayPath);
    return true;
}

} // namespace playback::ui
