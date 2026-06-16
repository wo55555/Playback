#include "Recorder.h"

#include "playback/Playback.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace playback::functions {

namespace {

constexpr char const* REPLAY_META_FILE = "playback_meta.json";

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

std::optional<PlaybackMeta> readPlaybackMeta(std::filesystem::path const& file) {
    getLogger().info("Trying to read metadata json {}", file);

    std::ifstream metadata(file, std::ios::binary);
    if (!metadata.is_open()) {
        getLogger().error("Metadata JSON doesn't exist!");
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << metadata.rdbuf();

    try {
        std::string json = buffer.str();
        if (json.empty()) {
            getLogger().error("Metadata JSON is empty");
            return std::nullopt;
        }
        return PlaybackMeta::fromJson(json);
    } catch (std::exception const& e) {
        getLogger().error("Failed to read playback metadata: {}", e.what());
        return std::nullopt;
    }
}

} // namespace

bool ReplayExporter::saveReplayData(const std::filesystem::path& replayPath) {
    if (!std::filesystem::exists(replayPath)) return false;

    PlaybackMeta meta;
    meta.worldName.clear();
    return writePlaybackMeta(replayPath, meta);
}

bool ReplayExporter::writePlaybackMeta(const std::filesystem::path& replayPath, const PlaybackMeta& meta) {
    std::error_code ec;
    std::filesystem::create_directories(replayPath, ec);
    if (ec) return false;

    std::ofstream file(replayPath / REPLAY_META_FILE, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;

    file << meta.toJson();
    return file.good();
}

} // namespace playback::functions
