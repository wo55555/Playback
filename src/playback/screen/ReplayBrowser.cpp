#include "ReplayBrowser.h"

#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/refactor/editor/Editor.h"
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
#include <windows.h>
#include <shellapi.h>

namespace playback::screen {

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

    // 名称降级策略：读取不到元数据名称（缺文件、名为空或默认占位符）时，使用文件名（不含扩展名）作为名称。
    std::string const fileStem = entry.path().stem().string();

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
        summary.replayName = fileStem;
        summary.problem    = "missing metadata.json";
        return summary;
    }

    try {
        auto meta             = playback::functions::PlaybackMeta::fromJson(*metadata);
        summary.replayName    = (meta.name.empty() || meta.name == "Unnamed") ? fileStem : std::move(meta.name);
        summary.worldName     = std::move(meta.worldName);
        summary.durationTicks = meta.duration;
        summary.totalTicks    = meta.totalTicks;
        summary.canOpen       = true;
        if (auto thumbnail = readZipEntry(summary.path, "icon.png")) {
            summary.thumbnailPng = std::move(*thumbnail);
        }
    } catch (std::exception const& e) {
        summary.replayName = fileStem;
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

// 用新内容替换 zip 归档中的指定条目（就地修改，不影响其他条目）。
bool updateZipEntry(
    std::filesystem::path const& archivePath,
    std::string const&           entryName,
    std::string const&           content,
    std::string&                 error
) {
    auto pathString = archivePath.string();
    int  zipError   = 0;
    auto archive    = zip_open(pathString.c_str(), ZIP_CREATE, &zipError);
    if (archive == nullptr) {
        zip_error_t entryError;
        zip_error_init_with_code(&entryError, zipError);
        error = zip_error_strerror(&entryError);
        zip_error_fini(&entryError);
        return false;
    }

    zip_int64_t index = zip_name_locate(archive, entryName.c_str(), 0);
    if (index < 0) {
        error = "回放归档中缺少 " + entryName;
        zip_discard(archive);
        return false;
    }

    auto* source = zip_source_buffer(archive, content.data(), content.size(), 0);
    if (source == nullptr) {
        error = zip_strerror(archive);
        zip_discard(archive);
        return false;
    }

    if (zip_file_replace(archive, index, source, 0) < 0) {
        error = zip_strerror(archive);
        zip_source_free(source);
        zip_discard(archive);
        return false;
    }

    if (zip_close(archive) < 0) {
        error = zip_strerror(archive);
        zip_discard(archive);
        return false;
    }
    return true;
}

// 去掉开头/结尾空白，并过滤文件名非法字符。
std::string sanitizeReplayName(std::string_view input) {
    std::string const cleanedRaw(input);
    auto const first = cleanedRaw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto const last = cleanedRaw.find_last_not_of(" \t\r\n");
    std::string cleaned = cleanedRaw.substr(first, last - first + 1);

    std::string result;
    result.reserve(cleaned.size());
    for (char ch : cleaned) {
        switch (ch) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|': continue;
        default: result.push_back(ch);
        }
    }
    while (!result.empty() && (result.back() == '.' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

} // namespace

std::string ReplaySummary::displayName() const {
    if (!replayName.empty() && replayName != "Unnamed") return replayName;
    return path.stem().string();
}

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
    getLogger().info("ReplayBrowser::openReplay: opening {}", replayPath.string());

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

    // Open the refactored editor when entering replay mode via the menu
    playback::refactor::editor::Editor::getInstance().open();

    return true;
}

bool ReplayBrowser::importReplay(std::filesystem::path const& source, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!hasReplayExtension(source) || !std::filesystem::is_regular_file(source, ec)) {
        error = "请选择有效的 .playback 或 .zip 回放文件";
        return false;
    }
    auto directory = utils::PathUtils::getReplaysDir();
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = ec.message(); return false; }
    auto destination = directory / source.filename();
    int suffix = 1;
    while (std::filesystem::exists(destination, ec)) {
        destination = directory / (source.stem().string() + " (" + std::to_string(suffix++) + ")" + source.extension().string());
    }
    if (ec) { error = ec.message(); return false; }
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
    if (ec) { error = ec.message(); return false; }
    auto imported = findReplay(destination.string());
    if (!imported || !imported->canOpen) {
        std::filesystem::remove(destination, ec);
        error = "文件不是有效的回放归档";
        return false;
    }
    return true;
}

bool ReplayBrowser::deleteReplay(ReplaySummary const& replay, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::remove(replay.path, ec)) {
        error = ec ? ec.message() : "回放文件不存在";
        return false;
    }
    return true;
}

bool ReplayBrowser::showInFolder(ReplaySummary const& replay) {
    // 使用绝对路径，避免相对路径下资源管理器无法定位文件。
    std::error_code ec;
    auto const path = std::filesystem::absolute(replay.path, ec);
    if (ec) return false;

    auto const wpath   = path.wstring();
    auto const wparent = path.parent_path().wstring();

    auto const result = reinterpret_cast<intptr_t>(ShellExecuteW(
        nullptr,
        L"open",
        L"explorer.exe",
        (L"/select,\"" + wpath + L"\"").c_str(),
        wparent.c_str(),
        SW_SHOWNORMAL
    ));

    // 文件定位失败时，回退为直接打开父目录。
    if (result <= 32) {
        return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", wparent.c_str(), nullptr, nullptr, SW_SHOWNORMAL))
            > 32;
    }
    return true;
}

bool ReplayBrowser::renameReplay(ReplaySummary const& replay, std::string_view newName, std::string& error) {
    error.clear();

    auto const name = sanitizeReplayName(newName);
    if (name.empty()) {
        error = "回放名称不能为空";
        return false;
    }

    // 1. 读取归档内元数据并更新名称字段；失败则中止。
    auto const metadata = readZipEntry(replay.path, "metadata.json");
    if (!metadata.has_value()) {
        error = "回放归档中缺少 metadata.json，无法重命名";
        return false;
    }

    std::string updatedJson;
    try {
        auto meta = playback::functions::PlaybackMeta::fromJson(*metadata);
        meta.name = name;
        updatedJson = meta.toJson();
    } catch (std::exception const& e) {
        error = std::string("无法解析回放元数据: ") + e.what();
        return false;
    }

    if (updatedJson != *metadata && !updateZipEntry(replay.path, "metadata.json", updatedJson, error)) {
        return false;
    }

    // 2. 重命名物理文件；失败时回滚元数据写入。
    auto const newPath = replay.path.parent_path() / (name + ".playback");
    if (newPath != replay.path) {
        std::error_code ec;
        std::filesystem::rename(replay.path, newPath, ec);
        if (ec) {
            if (updatedJson != *metadata) {
                std::string rollbackError;
                updateZipEntry(replay.path, "metadata.json", *metadata, rollbackError);
            }
            error = "重命名文件失败: " + ec.message();
            return false;
        }
    }

    return true;
}

} // namespace playback::screen
