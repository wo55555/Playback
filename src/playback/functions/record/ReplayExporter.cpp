#include "Recorder.h"

#include "playback/Playback.h"

#include "zip.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace playback::functions {

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

std::string toZipPath(std::filesystem::path const& path) { return path.generic_string(); }

bool setBestSpeed(zip_t* zip, zip_int64_t index) {
    return zip_set_file_compression(zip, static_cast<zip_uint64_t>(index), ZIP_CM_DEFLATE, 1) == 0;
}

bool writeBufferEntry(zip_t* zip, std::string const& entryName, std::string const& data) {
    auto& logger = getLogger();

    auto* source = zip_source_buffer(zip, data.data(), data.size(), 0);
    if (source == nullptr) {
        logger.error("Unable to create zip source for {}: {}", entryName, zip_strerror(zip));
        return false;
    }

    auto index = zip_file_add(zip, entryName.c_str(), source, ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(source);
        logger.error("Unable to add {} to zip: {}", entryName, zip_strerror(zip));
        return false;
    }

    if (!setBestSpeed(zip, index)) {
        logger.warn("Unable to set compression level for {}: {}", entryName, zip_strerror(zip));
    }
    return true;
}

bool writeFileEntry(zip_t* zip, std::filesystem::path const& file, std::string const& entryName) {
    auto& logger   = getLogger();
    auto  filePath = file.string();

    std::error_code ec;
    auto            fileSize = std::filesystem::file_size(file, ec);
    if (ec) {
        logger.error("Unable to stat {} for zip export: {}", file, ec.message());
        return false;
    }

    auto* source = zip_source_file(zip, filePath.c_str(), 0, static_cast<zip_int64_t>(fileSize));
    if (source == nullptr) {
        logger.error("Unable to create zip source for {}: {}", file, zip_strerror(zip));
        return false;
    }

    auto index = zip_file_add(zip, entryName.c_str(), source, ZIP_FL_OVERWRITE);
    if (index < 0) {
        zip_source_free(source);
        logger.error("Unable to add {} to zip: {}", file, zip_strerror(zip));
        return false;
    }

    if (!setBestSpeed(zip, index)) {
        logger.warn("Unable to set compression level for {}: {}", entryName, zip_strerror(zip));
    }
    return true;
}

} // namespace

bool ReplayExporter::exportReplay(
    std::filesystem::path const& recordDir,
    std::filesystem::path const& outputFile,
    std::string_view             name
) {
    auto& logger = getLogger();
    logger.info("Exporting {} to {}", recordDir, outputFile);

    auto meta = tryReadMeta(recordDir / "metadata.json");

    if (!meta.has_value()) meta = tryReadMeta(recordDir / "metadata.json.old");
    if (!meta.has_value()) {
        logger.error("Cannot export, both metadata files are invalid");
        return false;
    }

    if (!name.empty()) meta->name = name;

    for (auto it = meta->chunks.begin(); it != meta->chunks.end();) {
        auto const& chunkName = it->first;
        auto        chunkPath = recordDir / chunkName;

        if (!std::filesystem::exists(chunkPath)) {
            logger.warn("Cannot find chunk path: {}, skipping", chunkPath);
            it = meta->chunks.erase(it);
        } else {
            ++it;
        }

        if (meta->chunks.empty()) {
            logger.error("Cannot export, no chunk files exist");
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(outputFile.parent_path(), ec);
    if (ec) {
        logger.error("Unable to create parent directories", ec);
    }

    auto outputPath = outputFile.string();
    int  zipError   = 0;
    auto zip        = zip_open(outputPath.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipError);
    if (zip == nullptr) {
        zip_error_t error;
        zip_error_init_with_code(&error, zipError);
        logger.error("Exception exporting replay: {}", zip_error_strerror(&error));
        zip_error_fini(&error);
        return false;
    }

    auto closeZip = true;

    // Write metadata
    std::string metadataJson = meta->toJson();
    if (!writeBufferEntry(zip, "metadata.json", metadataJson)) {
        closeZip = false;
    }

    // Write chunked level chunk caches
    auto levelChunkCaches = recordDir / "level_chunk_caches";
    if (std::filesystem::exists(levelChunkCaches) && std::filesystem::is_directory(levelChunkCaches)) {
        for (auto const& entry : std::filesystem::directory_iterator(levelChunkCaches)) {
            if (!entry.is_regular_file()) continue;

            auto entryName = toZipPath(std::filesystem::path("level_chunk_caches") / entry.path().filename());
            if (!writeFileEntry(zip, entry.path(), entryName)) {
                closeZip = false;
                break;
            }
        }
    }

    // Write level chunk cache
    auto levelChunkCachePath = recordDir / "level_chunk_cache";
    if (closeZip && std::filesystem::exists(levelChunkCachePath)) {
        closeZip = writeFileEntry(zip, levelChunkCachePath, "level_chunk_cache");
    }

    // Write icon
    auto iconPath = recordDir / "icon.png";
    if (closeZip && std::filesystem::exists(iconPath)) {
        closeZip = writeFileEntry(zip, iconPath, "icon.png");
    }

    // Write chunks
    if (closeZip) {
        for (auto const& [chunkName, _] : meta->chunks) {
            auto chunkPath = recordDir / chunkName;
            if (!writeFileEntry(zip, chunkPath, chunkName)) {
                closeZip = false;
                break;
            }
        }
    }

    if (closeZip) {
        if (zip_close(zip) < 0) {
            logger.error("Exception exporting replay: {}", zip_strerror(zip));
            zip_discard(zip);
            return false;
        }
    } else {
        zip_discard(zip);
        return false;
    }

    ec.clear();
    std::filesystem::remove_all(recordDir, ec);
    if (ec) {
        logger.error("Exception deleting record folder: {}", ec.message());
    }
    return true;
}

std::optional<PlaybackMeta> ReplayExporter::tryReadMeta(std::filesystem::path const& file) {
    getLogger().debug("Trying to read metadata json {}", file);

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

} // namespace playback::functions
