#include "ReplayResourcePacks.h"

#include "playback/Playback.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/core/file/Path.h"
#include "mc/deps/core/resource/PackIdVersion.h"
#include "mc/deps/core/resource/PackOrigin.h"
#include "mc/deps/core/resource/PackType.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/sem_ver/SemVersion.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/packet/PackInfoData.h"
#include "mc/network/packet/PacksInfoData.h"
#include "mc/network/packet/ResourcePacksInfoPacket.h"
#include "mc/platform/UUID.h"
#include "mc/world/level/PackInstanceId.h"
#include "mc/resources/CompositePackSource.h"
#include "mc/resources/DirectoryPackSource.h"
#include "mc/resources/Pack.h"
#include "mc/resources/PackManifest.h"
#include "mc/resources/PackSourceFactory.h"
#include "mc/resources/RepositorySources.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackRepository.h"

#include "xxhash.h"
#include "zip.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace playback::io::packs {

namespace {

auto& getLogger() { return Playback::getInstance().getSelf().getLogger(); }

constexpr std::string_view ArchiveRoot     = "resource_packs/";
constexpr std::string_view IndexEntry      = "resource_packs/index.json";
constexpr int              IndexFormat     = 1;
// Kept beside resource_packs/ so the local cache paths it lists never reach the exported replay.
constexpr std::string_view ServerPacksFile = "server_packs.json";

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(text.data()), text.size()));
}

std::string toUtf8(std::filesystem::path const& path) {
    auto text = path.generic_u8string();
    return std::string(reinterpret_cast<char const*>(text.data()), text.size());
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string packKey(std::string_view uuid, std::string_view version) { return lowercase(uuid) + "_" + std::string(version); }

std::string packKey(PackIdVersion const& identity) {
    return packKey(identity.mId->asString(), identity.mVersion->asString());
}

bool isCacheOrigin(PackOrigin origin) { return origin == PackOrigin::TempCache || origin == PackOrigin::PremiumTempCache; }

// Short on purpose: pack-internal paths run close to MAX_PATH, and the game reads the mount without long-path support.
std::string folderFor(std::string const& key) {
    return std::format("{:012x}", XXH3_64bits(key.data(), key.size()) & 0xFFFFFFFFFFFFull);
}

bool isReservedDeviceName(std::string_view component) {
    auto stem = component.substr(0, component.find('.'));
    std::string upper(stem);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
    if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0'
        && upper[3] <= '9') {
        return true;
    }
    return false;
}

// Archive names are untrusted: only plain relative components may reach the file system.
bool isSafeComponent(std::string_view component) {
    if (component.empty() || component == "." || component == "..") return false;
    if (component.back() == '.' || component.back() == ' ') return false;
    for (unsigned char c : component) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '\\' || c == '|' || c == '?'
            || c == '*') {
            return false;
        }
    }
    return !isReservedDeviceName(component);
}

bool isFolderName(std::string_view folder) {
    return isSafeComponent(folder) && std::ranges::all_of(folder, [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_';
    });
}

std::optional<std::vector<std::string_view>> splitSafePath(std::string_view relative) {
    if (relative.empty()) return std::nullopt;
    std::vector<std::string_view> components;
    size_t                        start = 0;
    while (start <= relative.size()) {
        auto end       = relative.find('/', start);
        auto component = relative.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!isSafeComponent(component)) return std::nullopt;
        components.push_back(component);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return components;
}

struct CopyResult {
    uint64_t files{};
    uint64_t bytes{};
};

// Copies a downloaded pack as is. The destination is removed again on any failure.
std::optional<CopyResult> copyPackDirectory(std::filesystem::path const& source, std::filesystem::path const& destination) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source / "manifest.json", ec)) {
        getLogger().warn("Resource pack {} has no manifest.json; skipping", source);
        return std::nullopt;
    }

    auto fail = [&destination](std::string_view reason) -> std::optional<CopyResult> {
        getLogger().warn("Unable to copy resource pack to {}: {}", destination, reason);
        std::error_code removeError;
        std::filesystem::remove_all(destination, removeError);
        return std::nullopt;
    };

    CopyResult result;
    for (auto it = std::filesystem::recursive_directory_iterator(source, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        auto target = destination / std::filesystem::relative(it->path(), source, ec);
        if (ec) break;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) break;
        std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) break;
        result.bytes += it->file_size(ec);
        ++result.files;
    }
    if (ec) return fail(ec.message());
    return result;
}

enum class ZipPackStatus { Ok, Cancel, Fail };

struct ZipPackResult {
    ZipPackStatus status{ZipPackStatus::Fail};
    uint64_t      files{};
    uint64_t      bytes{};
};

// One pack per zip. STORE only: the files are already compressed or encrypted.
ZipPackResult zipPackDirectory(
    std::filesystem::path const& source,
    std::filesystem::path const& destination,
    std::stop_token              stop
) {
    ZipPackResult result;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source / "manifest.json", ec)) {
        getLogger().warn("Resource pack {} has no manifest.json; skipping", source);
        return result;
    }

    auto part = destination;
    part += ".part";
    std::filesystem::remove(part, ec);
    std::filesystem::remove(destination, ec);

    auto discardPart = [&] {
        std::error_code removeError;
        std::filesystem::remove(part, removeError);
    };

    int zipError = 0;
    auto* archive = zip_open(toUtf8(part).c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zipError);
    if (!archive) {
        zip_error_t error;
        zip_error_init_with_code(&error, zipError);
        getLogger().warn("Unable to create {}: {}", part, zip_error_strerror(&error));
        zip_error_fini(&error);
        return result;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(source, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (stop.stop_requested()) {
            zip_discard(archive);
            discardPart();
            result.status = ZipPackStatus::Cancel;
            return result;
        }
        if (!it->is_regular_file(ec)) continue;

        auto relative = std::filesystem::relative(it->path(), source, ec);
        if (ec) break;
        auto entryName = toUtf8(relative);
        std::ranges::replace(entryName, '\\', '/');
        auto size = it->file_size(ec);
        if (ec) break;

        auto* buffer = static_cast<char*>(std::malloc(size == 0 ? 1 : static_cast<size_t>(size)));
        if (!buffer) {
            zip_discard(archive);
            discardPart();
            getLogger().warn("Unable to read {} into memory", it->path());
            return result;
        }
        {
            std::ifstream input(it->path(), std::ios::binary);
            if (size > 0) input.read(buffer, static_cast<std::streamsize>(size));
            if (!input) {
                std::free(buffer);
                zip_discard(archive);
                discardPart();
                getLogger().warn("Unable to read {}", it->path());
                return result;
            }
        }

        auto* sourceBuffer = zip_source_buffer(archive, buffer, size, 1);
        if (!sourceBuffer) {
            std::free(buffer);
            zip_discard(archive);
            discardPart();
            getLogger().warn("Unable to create zip source for {}: {}", entryName, zip_strerror(archive));
            return result;
        }
        auto index = zip_file_add(archive, entryName.c_str(), sourceBuffer, ZIP_FL_ENC_UTF_8);
        if (index < 0) {
            zip_source_free(sourceBuffer);
            zip_discard(archive);
            discardPart();
            getLogger().warn("Unable to add {} to {}: {}", entryName, part, zip_strerror(archive));
            return result;
        }
        if (zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0) != 0) {
            getLogger().warn("Unable to store {} uncompressed: {}", entryName, zip_strerror(archive));
        }
        result.bytes += size;
        ++result.files;
    }
    if (ec) {
        zip_discard(archive);
        discardPart();
        getLogger().warn("Unable to read resource pack {}: {}", source, ec.message());
        return result;
    }

    if (zip_close(archive) < 0) {
        getLogger().warn("Unable to close {}: {}", part, zip_strerror(archive));
        zip_discard(archive);
        discardPart();
        return result;
    }

    std::filesystem::rename(part, destination, ec);
    if (ec) {
        discardPart();
        getLogger().warn("Unable to finalize {}: {}", destination, ec.message());
        return result;
    }
    result.status = ZipPackStatus::Ok;
    return result;
}

// Reads the uuid_version key from a pack's manifest.json header.
std::optional<std::string> manifestKey(std::filesystem::path const& packDir) {
    std::ifstream input(packDir / "manifest.json", std::ios::binary);
    if (!input) return std::nullopt;
    auto json = nlohmann::json::parse(input, nullptr, false, true);
    if (json.is_discarded() || !json.is_object() || !json.contains("header") || !json["header"].is_object()) {
        return std::nullopt;
    }
    auto const& header = json["header"];
    auto        uuid   = header.value("uuid", std::string{});
    if (uuid.empty() || !header.contains("version")) return std::nullopt;

    std::string version;
    auto const& raw = header["version"];
    if (raw.is_array() && raw.size() == 3 && raw[0].is_number_integer() && raw[1].is_number_integer()
        && raw[2].is_number_integer()) {
        version = std::format("{}.{}.{}", raw[0].get<int64_t>(), raw[1].get<int64_t>(), raw[2].get<int64_t>());
    } else if (raw.is_string()) {
        version = raw.get<std::string>();
    } else {
        return std::nullopt;
    }
    return packKey(uuid, version);
}

std::shared_ptr<ResourcePacksInfoPacket>
decodeLatestPacksInfo(std::vector<PlaybackSerializedGamePacket> const& packets) {
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
        if (it->mPacketId != static_cast<int32_t>(MinecraftPacketIds::ResourcePacksInfo)) continue;
        auto packet = MinecraftPackets::createPacket(MinecraftPacketIds::ResourcePacksInfo);
        if (!packet) return nullptr;
        ReadOnlyBinaryStream stream(it->mPayload, false);
        if (!packet->read(stream) || !stream.ensureReadCompleted()) return nullptr;
        return std::static_pointer_cast<ResourcePacksInfoPacket>(packet);
    }
    return nullptr;
}

std::optional<nlohmann::json> readJsonFile(std::filesystem::path const& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    auto json = nlohmann::json::parse(input, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return std::nullopt;
    return json;
}

bool writeJsonFile(std::filesystem::path const& file, nlohmann::json const& json) {
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << json.dump(2);
        if (!output) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, file, ec);
    return !ec;
}

std::optional<std::string> readEntry(zip_t* archive, zip_uint64_t index, uint64_t expectedSize) {
    std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(archive, index, 0), &zip_fclose);
    if (!file) return std::nullopt;
    std::string data(static_cast<size_t>(expectedSize), '\0');
    size_t      offset = 0;
    while (offset < data.size()) {
        auto read = zip_fread(file.get(), data.data() + offset, data.size() - offset);
        if (read <= 0) return std::nullopt;
        offset += static_cast<size_t>(read);
    }
    char extra{};
    if (zip_fread(file.get(), &extra, 1) != 0) return std::nullopt;
    return data;
}

struct EmbeddedIndex {
    std::unordered_set<std::string> folders;
    std::unordered_set<std::string> keys;
    size_t                          files{};
};

EmbeddedIndex parseIndex(std::string const& text) {
    EmbeddedIndex index;
    auto          json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("packs") || !json["packs"].is_array()) {
        throw std::runtime_error("resource_packs/index.json is malformed");
    }
    for (auto const& pack : json["packs"]) {
        if (!pack.is_object()) continue;
        auto folder  = pack.value("folder", std::string{});
        auto uuid    = pack.value("uuid", std::string{});
        auto version = pack.value("version", std::string{});
        if (!isFolderName(folder) || uuid.empty() || version.empty()) continue;
        index.folders.insert(folder);
        index.keys.insert(packKey(uuid, version));
        auto const files = pack.value("files", nlohmann::json(0));
        if (files.is_number_unsigned()) index.files += files.get<size_t>();
    }
    return index;
}

struct ExtractEntry {
    zip_uint64_t          index{};
    uint64_t              size{};
    std::filesystem::path target;
};

void writeExtractedEntry(zip_t* archive, ExtractEntry const& entry) {
    auto data = readEntry(archive, entry.index, entry.size);
    if (!data) throw std::runtime_error("Unable to read resource pack entry for " + toUtf8(entry.target));
    std::ofstream output(entry.target, std::ios::binary | std::ios::trunc);
    output.write(data->data(), static_cast<std::streamsize>(data->size()));
    if (!output) throw std::runtime_error("Unable to write " + toUtf8(entry.target));
}

// Extracts resource_packs/<folder>/** into destination. Throws on anything that looks like a path escape.
// Names are checked and directories created up front; the file writes, which dominate, run on a few threads that
// each open their own read-only handle (a libzip archive is not thread-safe).
size_t extractEmbeddedPacks(
    zip_t*                       archive,
    std::string const&           archivePath,
    EmbeddedIndex const&         index,
    std::filesystem::path const& destination
) {
    std::unordered_set<std::string>  seen;
    std::vector<ExtractEntry>        entries;
    std::set<std::filesystem::path>  directories;
    auto const                       count = zip_get_num_entries(archive, 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        auto const* rawName = zip_get_name(archive, static_cast<zip_uint64_t>(i), 0);
        if (!rawName) continue;
        std::string_view name(rawName);
        if (!name.starts_with(ArchiveRoot) || name == IndexEntry || name.ends_with('/')) continue;

        auto relative   = name.substr(ArchiveRoot.size());
        auto components = splitSafePath(relative);
        if (!components || components->size() < 2) {
            throw std::runtime_error("Unsafe resource pack entry name: " + std::string(name));
        }
        if (!index.folders.contains(std::string((*components)[0]))) continue;
        if (!seen.insert(lowercase(relative)).second) {
            throw std::runtime_error("Duplicate resource pack entry: " + std::string(name));
        }

        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE)) {
            throw std::runtime_error("Unable to stat resource pack entry " + std::string(name));
        }

        auto target = destination / fromUtf8(relative);
        directories.insert(target.parent_path());
        entries.push_back({static_cast<zip_uint64_t>(i), stat.size, std::move(target)});
    }

    for (auto const& directory : directories) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) throw std::runtime_error("Unable to create " + toUtf8(directory) + ": " + ec.message());
    }

    auto const hardware = std::max(2u, std::thread::hardware_concurrency());
    auto const workers  = std::min<size_t>({4, hardware / 2, std::max<size_t>(1, entries.size() / 64)});
    if (workers <= 1) {
        for (auto const& entry : entries) writeExtractedEntry(archive, entry);
        return entries.size();
    }

    std::atomic<size_t> next{0};
    std::atomic<bool>   failed{false};
    std::mutex          errorMutex;
    std::string         error;
    auto                fail = [&](std::string message) {
        std::scoped_lock lock(errorMutex);
        if (error.empty()) error = std::move(message);
        failed.store(true, std::memory_order_relaxed);
    };
    {
        std::vector<std::jthread> threads;
        threads.reserve(workers);
        for (size_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&] {
                int zipError = 0;
                std::unique_ptr<zip_t, decltype(&zip_close)> handle(
                    zip_open(archivePath.c_str(), ZIP_RDONLY, &zipError),
                    &zip_close
                );
                if (!handle) {
                    fail(std::format("Unable to reopen the replay archive ({})", zipError));
                    return;
                }
                while (!failed.load(std::memory_order_relaxed)) {
                    auto const position = next.fetch_add(1, std::memory_order_relaxed);
                    if (position >= entries.size()) return;
                    try {
                        writeExtractedEntry(handle.get(), entries[position]);
                    } catch (std::exception const& exception) {
                        fail(exception.what());
                    } catch (...) {
                        fail("Unable to extract a resource pack entry");
                    }
                }
            });
        }
    }
    if (failed.load()) throw std::runtime_error(error);
    return entries.size();
}

// Extracted packs are kept between loads of the same replay: the mount is renamed into the cache on release and back
// to a fresh mount path next time, one directory rename instead of thousands of file writes.
// Same length as replay_packs/<8 hex>: pack-internal paths already run close to MAX_PATH.
constexpr std::string_view CacheDirName      = "replay_cache";
constexpr std::string_view CacheCompleteFile = "complete";
constexpr std::string_view CacheKeyFile      = "cache_key";
constexpr int              CacheFormat       = 1;
constexpr size_t           CacheEntries      = 3;

std::filesystem::path cacheParent() { return utils::PathUtils::getSharedTempDir() / std::string(CacheDirName); }

std::string cacheKeyFor(std::string const& indexText, std::filesystem::path const& replayFile) {
    std::error_code ec;
    auto const      size  = std::filesystem::file_size(replayFile, ec);
    auto const      mtime = std::filesystem::last_write_time(replayFile, ec).time_since_epoch().count();
    auto const      key   = std::format("{}|{}|{}|{}", CacheFormat, size, mtime, indexText);
    return std::format("{:08x}", static_cast<uint32_t>(XXH3_64bits(key.data(), key.size())));
}

bool cacheEntryComplete(std::filesystem::path const& entry) {
    std::error_code ec;
    return std::filesystem::is_regular_file(entry / std::string(CacheCompleteFile), ec)
        && std::filesystem::is_directory(entry / "embedded", ec);
}

// Drops unfinished entries and keeps the most recently used complete ones.
void pruneCache(std::filesystem::path const& keep) {
    std::error_code                    ec;
    std::vector<std::filesystem::path> children;
    for (auto const& entry : std::filesystem::directory_iterator(cacheParent(), ec)) children.push_back(entry.path());

    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> complete;
    for (auto const& path : children) {
        if (path == keep) continue;
        if (!cacheEntryComplete(path)) {
            std::filesystem::remove_all(path, ec);
            continue;
        }
        complete.emplace_back(std::filesystem::last_write_time(path / std::string(CacheCompleteFile), ec), path);
    }
    std::ranges::sort(complete, std::greater{}, &decltype(complete)::value_type::first);
    for (size_t index = CacheEntries > 0 ? CacheEntries - 1 : 0; index < complete.size(); ++index) {
        std::filesystem::remove_all(complete[index].second, ec);
    }
}

nlohmann::json readIndexFile(std::filesystem::path const& root) {
    auto index = readJsonFile(root / "index.json");
    if (index && (*index)["packs"].is_array()) return *index;
    return {{"formatVersion", IndexFormat}, {"packs", nlohmann::json::array()}};
}

bool packAlreadyPresent(std::filesystem::path const& root, std::string const& folder) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(root / (folder + ".zip"), ec)) return true;
    return std::filesystem::is_directory(root / folder, ec);
}

bool embedPack(
    std::filesystem::path const& root,
    nlohmann::json&              index,
    ServerPackSource const&      pack,
    std::filesystem::path const& source,
    std::stop_token              stop
) {
    auto zipped = zipPackDirectory(source, root / (pack.folder + ".zip"), stop);
    if (zipped.status != ZipPackStatus::Ok) return false;
    index["packs"].push_back({
        {"uuid",    pack.uuid    },
        {"version", pack.version },
        {"folder",  pack.folder  },
        {"files",   zipped.files },
        {"bytes",   zipped.bytes },
    });
    return true;
}

bool writeIndexFile(std::filesystem::path const& root, nlohmann::json const& index) {
    if (!writeJsonFile(root / "index.json", index)) {
        getLogger().warn("Unable to write the embedded resource pack index");
        return false;
    }
    return true;
}

} // namespace

ServerPackSnapshot collectServerPacks(std::vector<PlaybackSerializedGamePacket> const& configurationPackets) {
    ServerPackSnapshot result;
    auto               info = decodeLatestPacksInfo(configurationPackets);
    auto               client = ll::service::getClientInstance();
    if (!info || info->mData->mResourcePacks->empty() || !client) return result;

    std::unordered_set<std::string> announced;
    for (auto const& pack : *info->mData->mResourcePacks) announced.insert(packKey(*pack.mPackIdVersion));

    auto& repository = client->getResourcePackRepository();
    std::unordered_map<std::string, ServerPackSource> loaded;
    repository.forEachPack([&](ResourcePack const& pack) {
        auto const& manifest = *pack.mPack.get()->mManifest.get();
        if (manifest.mPackType != PackType::Resources) return;
        auto key = packKey(*manifest.mIdentity);
        if (!announced.contains(key)) return;
        // A locally installed pack with the same identity only counts when the server copy is not loaded.
        if (auto it = loaded.find(key); it != loaded.end() && !isCacheOrigin(manifest.mPackOrigin)) return;
        loaded[key] = {
            manifest.mIdentity->mId->asString(),
            manifest.mIdentity->mVersion->asString(),
            folderFor(key),
            fromUtf8(manifest.mLocation->getFullPath().value),
            static_cast<int>(manifest.mPackOrigin)
        };
    });

    for (auto const& pack : *info->mData->mResourcePacks) {
        auto key = packKey(*pack.mPackIdVersion);
        if (auto it = loaded.find(key); it != loaded.end()) {
            result.announced.push_back(it->second);
        } else {
            result.announced.push_back(
                {pack.mPackIdVersion->mId->asString(), pack.mPackIdVersion->mVersion->asString(), folderFor(key), {}}
            );
        }
    }

    // addCachedResourcePacks only ever puts directory sources into the cache composite.
    auto& sources = static_cast<ResourcePackRepository&>(repository).mSources->mCachePackSource;
    if (sources) {
        for (auto* source : *sources->mPackSources) {
            if (source->getPackType() != PackType::Resources || !isCacheOrigin(source->getPackOrigin())) continue;
            result.cacheDirectories.push_back(
                fromUtf8(static_cast<DirectoryPackSource*>(source)->mPath->get())
            );
        }
    }
    return result;
}

EmbedJobResult embedServerPacks(
    std::filesystem::path const& recordDir,
    ServerPackSnapshot const&    snapshot,
    std::stop_token              stop
) {
    EmbedJobResult result;
    try {
        auto            root = recordDir / "resource_packs";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) {
            getLogger().warn("Unable to create {}: {}", root, ec.message());
            return result;
        }

        auto index = readIndexFile(root);
        for (auto const& pack : snapshot.announced) {
            if (stop.stop_requested()) break;
            if (pack.directory.empty()) continue;
            if (packAlreadyPresent(root, pack.folder)) {
                ++result.present;
                continue;
            }
            if (embedPack(root, index, pack, pack.directory, stop)) {
                ++result.zipped;
                writeIndexFile(root, index);
            } else {
                ++result.failed;
            }
        }

        auto server = readJsonFile(recordDir / ServerPacksFile)
                          .value_or(nlohmann::json{{"packs", nlohmann::json::array()},
                                                   {"cacheDirectories", nlohmann::json::array()}});
        auto changed = false;
        for (auto const& pack : snapshot.announced) {
            nlohmann::json entry{{"uuid", pack.uuid}, {"version", pack.version}, {"folder", pack.folder}};
            if (std::ranges::find(server["packs"], entry) != server["packs"].end()) continue;
            server["packs"].push_back(std::move(entry));
            changed = true;
        }
        for (auto const& directory : snapshot.cacheDirectories) {
            auto  text  = toUtf8(directory);
            auto& known = server["cacheDirectories"];
            if (std::find(known.begin(), known.end(), text) != known.end()) continue;
            server["cacheDirectories"].push_back(std::move(text));
            changed = true;
        }
        if (changed && !writeJsonFile(recordDir / ServerPacksFile, server)) {
            getLogger().warn("Unable to write the announced server resource pack list");
        }
        if (result.zipped > 0) getLogger().info("Embedded {} server resource packs", result.zipped);
    } catch (std::exception const& exception) {
        getLogger().warn("Unable to embed server resource packs: {}", exception.what());
    }
    return result;
}

size_t completeServerPacks(std::filesystem::path const& recordDir) {
    try {
        auto server = readJsonFile(recordDir / ServerPacksFile);
        if (!server || !(*server)["packs"].is_array()) return 0;

        auto                            root = recordDir / "resource_packs";
        auto                            index = readIndexFile(root);
        std::unordered_set<std::string> present;
        for (auto const& pack : index["packs"]) {
            present.insert(packKey(pack.value("uuid", std::string{}), pack.value("version", std::string{})));
        }

        std::unordered_map<std::string, ServerPackSource> missing;
        for (auto const& entry : (*server)["packs"]) {
            ServerPackSource pack{
                entry.value("uuid", std::string{}),
                entry.value("version", std::string{}),
                entry.value("folder", std::string{}),
                {}
            };
            auto key = packKey(pack.uuid, pack.version);
            if (pack.uuid.empty() || !isSafeComponent(pack.folder) || present.contains(key)) continue;
            missing.emplace(std::move(key), std::move(pack));
        }
        if (missing.empty()) return 0;

        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        size_t copied = 0;
        for (auto const& directory : (*server)["cacheDirectories"]) {
            if (missing.empty()) break;
            if (!directory.is_string()) continue;
            for (auto it = std::filesystem::directory_iterator(fromUtf8(directory.get<std::string>()), ec);
                 !ec && it != std::filesystem::directory_iterator() && !missing.empty();
                 it.increment(ec)) {
                if (!it->is_directory(ec)) continue;
                auto key = manifestKey(it->path());
                if (!key) continue;
                auto found = missing.find(*key);
                if (found == missing.end()) continue;
                if (embedPack(root, index, found->second, it->path(), {})) {
                    ++copied;
                    writeIndexFile(root, index);
                }
                missing.erase(found);
            }
            ec.clear();
        }
        if (copied > 0) getLogger().info("Embedded {} server resource packs from the local cache", copied);
        for (auto const& [key, _] : missing) {
            getLogger().warn("Server resource pack {} is not in the local cache; it is not embedded", key);
        }
        return missing.size();
    } catch (std::exception const& exception) {
        getLogger().warn("Unable to embed server resource packs from the local cache: {}", exception.what());
        return 0;
    }
}

std::optional<std::filesystem::path> mountReplayPacks(
    IResourcePackRepository&     repository,
    PacksInfoData const&         info,
    std::filesystem::path const& replayFile
) {
    auto startedAt = std::chrono::steady_clock::now();
    auto path      = replayFile.string();
    int  zipError  = 0;
    auto skip = [](std::string_view reason) -> std::optional<std::filesystem::path> {
        getLogger().debug("Replay embeds no resource packs: {}", reason);
        return std::nullopt;
    };
    std::unique_ptr<zip_t, decltype(&zip_close)> archive(zip_open(path.c_str(), ZIP_RDONLY, &zipError), &zip_close);
    if (!archive) return skip(std::format("zipOpen({})", zipError));

    auto indexPosition = zip_name_locate(archive.get(), IndexEntry.data(), 0);
    if (indexPosition < 0) return skip("noIndex");
    zip_stat_t indexStat;
    zip_stat_init(&indexStat);
    if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(indexPosition), 0, &indexStat) != 0) {
        throw std::runtime_error("resource_packs/index.json is unreadable");
    }
    auto indexText = readEntry(archive.get(), static_cast<zip_uint64_t>(indexPosition), indexStat.size);
    if (!indexText) throw std::runtime_error("resource_packs/index.json is unreadable");
    auto index = parseIndex(*indexText);
    if (index.folders.empty()) return skip("emptyIndex");

    auto& repo        = static_cast<ResourcePackRepository&>(repository);
    auto& cacheSource = repo.mSources->mCachePackSource;
    if (!cacheSource) throw std::runtime_error("The cache pack source is unavailable");
    auto& sources = *cacheSource->mPackSources;
    // addCachedResourcePacks has just rebuilt it from the four download cache directories.
    if (sources.size() != 4) {
        throw std::runtime_error(std::format("Unexpected cache pack source layout ({} sources)", sources.size()));
    }

    // The pack source factory keeps every directory source it created, so each mount gets a fresh path that has
    // never been discovered before. Mounts left behind by a crash are dropped here.
    auto parent = utils::PathUtils::getSharedTempDir() / "replay_packs";
    auto root   = parent
              / std::format(
                    "{:08x}",
                    static_cast<uint32_t>(
                        XXH3_64bits(indexText->data(), indexText->size())
                        ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
                    )
              );
    auto embedded   = root / "embedded";
    auto localCache = root / "local_cache";

    std::error_code ec;
    std::filesystem::remove_all(parent, ec);

    auto const cacheKey = cacheKeyFor(*indexText, replayFile);
    auto const cached   = cacheParent() / cacheKey;
    bool       fromCache = false;
    std::filesystem::create_directories(parent, ec);
    if (cacheEntryComplete(cached)) {
        std::error_code renameError;
        std::filesystem::rename(cached, root, renameError);
        fromCache = !renameError;
        if (renameError) {
            getLogger().warn("Unable to reuse the cached resource pack extraction: {}", renameError.message());
            std::filesystem::remove_all(cached, ec);
        }
    }

    size_t files = index.files;
    if (!fromCache) {
        std::filesystem::create_directories(embedded, ec);
        if (ec) throw std::runtime_error("Unable to create " + toUtf8(embedded) + ": " + ec.message());
        try {
            files = extractEmbeddedPacks(archive.get(), path, index, embedded);
        } catch (...) {
            std::filesystem::remove_all(root, ec);
            throw;
        }
    }
    archive.reset();
    // Release reads this to decide where the extraction is kept; the complete marker is only written then.
    std::filesystem::remove(root / std::string(CacheCompleteFile), ec);
    std::ofstream(root / std::string(CacheKeyFile), std::ios::binary | std::ios::trunc) << cacheKey;

    size_t mountedFolders = 0;
    for (auto const& folder : index.folders) {
        if (std::filesystem::is_regular_file(embedded / folder / "manifest.json", ec)) {
            ++mountedFolders;
        } else {
            std::filesystem::remove_all(embedded / folder, ec);
        }
    }
    if (mountedFolders == 0) {
        std::filesystem::remove_all(root, ec);
        return skip("noManifest");
    }

    std::unordered_set<std::string> announced;
    for (auto const& pack : *info.mResourcePacks) announced.insert(packKey(*pack.mPackIdVersion));

    // Packs the replay does not carry still come from the local cache, copied so the two sources never overlap.
    std::vector<std::pair<std::string, std::filesystem::path>> notEmbedded;
    size_t                                                     removed = 0;
    // 26.10 does not export _removePacksIf: collect first, then drop every cache-loaded pack. The sources rebuilt
    // below reload the cache packs that are not resource packs.
    repo.forEachPack([&](ResourcePack const& pack) {
        auto const& manifest = *pack.mPack.get()->mManifest.get();
        if (!isCacheOrigin(manifest.mPackOrigin) || manifest.mPackType != PackType::Resources) return;
        auto key = packKey(*manifest.mIdentity);
        if (!index.keys.contains(key) && announced.contains(key)) {
            notEmbedded.emplace_back(folderFor(key), fromUtf8(manifest.mLocation->getFullPath().value));
        }
        ++removed;
    });
    repo.removePacksLoadedFromCache();

    size_t fromLocalCache = 0;
    for (auto const& [folder, directory] : notEmbedded) {
        if (copyPackDirectory(directory, localCache / folder)) ++fromLocalCache;
    }

    std::vector<PackSource*> kept;
    for (auto* source : sources) {
        if (!isCacheOrigin(source->getPackOrigin()) || source->getPackType() != PackType::Resources) {
            kept.push_back(source);
        }
    }
    auto& factory = repository.getPackSourceFactory();
    kept.push_back(
        &factory.createDirectoryPackSource(Core::Path{embedded}, PackType::Resources, PackOrigin::TempCache, false)
    );
    if (fromLocalCache > 0) {
        kept.push_back(
            &factory.createDirectoryPackSource(Core::Path{localCache}, PackType::Resources, PackOrigin::TempCache, false)
        );
    }
    sources = std::move(kept);
    repo.refreshPacks();

    getLogger().info(
        "Mounted {} embedded resource packs ({} files{}, {} cached packs replaced, {} packs from the local cache) in "
        "{:.1f} ms",
        mountedFolders,
        files,
        fromCache ? ", reused" : "",
        removed,
        fromLocalCache,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count()
    );
    return root;
}

void removeMountedPacks(std::filesystem::path const& mountDir) {
    std::error_code ec;

    // Keep the extraction for the next load of the same replay; anything unexpected falls back to deleting it.
    std::string key;
    {
        std::ifstream input(mountDir / std::string(CacheKeyFile), std::ios::binary);
        std::getline(input, key);
    }
    bool cached = false;
    if (isFolderName(key)) {
        std::filesystem::remove_all(mountDir / "local_cache", ec);
        std::ofstream(mountDir / std::string(CacheCompleteFile), std::ios::binary | std::ios::trunc) << CacheFormat;
        auto const target = cacheParent() / key;
        std::filesystem::create_directories(cacheParent(), ec);
        std::filesystem::remove_all(target, ec);
        std::error_code renameError;
        std::filesystem::rename(mountDir, target, renameError);
        cached = !renameError;
        if (cached) pruneCache(target);
    }
    if (!cached) {
        ec.clear();
        std::filesystem::remove_all(mountDir, ec);
        if (ec) getLogger().warn("Unable to remove mounted resource packs {}: {}", mountDir, ec.message());
    }
}

namespace {

bool addStoredBuffer(zip_t* archive, std::string const& entryName, void* data, zip_uint64_t size) {
    auto* source = zip_source_buffer(archive, data, size, 1);
    if (!source) {
        std::free(data);
        getLogger().warn("Unable to create zip source for {}: {}", entryName, zip_strerror(archive));
        return false;
    }
    auto index = zip_file_add(archive, entryName.c_str(), source, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_source_free(source);
        getLogger().warn("Unable to add {} to zip: {}", entryName, zip_strerror(archive));
        return false;
    }
    if (zip_set_file_compression(archive, static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0) != 0) {
        getLogger().warn("Unable to store {} uncompressed: {}", entryName, zip_strerror(archive));
    }
    return true;
}

bool clonePackZip(zip_t* dest, std::filesystem::path const& packZip, std::string const& folder, PackExportStats& stats) {
    auto path     = toUtf8(packZip);
    int  zipError = 0;
    std::unique_ptr<zip_t, decltype(&zip_close)> source(zip_open(path.c_str(), ZIP_RDONLY, &zipError), &zip_close);
    if (!source) {
        getLogger().warn("Unable to open pack zip {}: {}", packZip, zipError);
        return false;
    }

    auto const count = zip_get_num_entries(source.get(), 0);
    for (zip_int64_t i = 0; i < count; ++i) {
        auto const* rawName = zip_get_name(source.get(), static_cast<zip_uint64_t>(i), 0);
        if (!rawName) continue;
        std::string_view name(rawName);
        if (name.ends_with('/')) continue;

        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(source.get(), static_cast<zip_uint64_t>(i), 0, &stat) != 0 || !(stat.valid & ZIP_STAT_SIZE)) {
            getLogger().warn("Unable to stat {} in {}", name, packZip);
            continue;
        }
        auto data = readEntry(source.get(), static_cast<zip_uint64_t>(i), stat.size);
        if (!data) {
            getLogger().warn("Unable to read {} from {}", name, packZip);
            continue;
        }
        auto  entryName = std::string(ArchiveRoot) + folder + "/" + std::string(name);
        auto* buffer    = static_cast<char*>(std::malloc(data->empty() ? 1 : data->size()));
        if (!buffer) continue;
        if (!data->empty()) std::memcpy(buffer, data->data(), data->size());
        if (addStoredBuffer(dest, entryName, buffer, data->size())) {
            stats.bytes += data->size();
            ++stats.entries;
        }
    }
    ++stats.packs;
    return true;
}

bool clonePackDirectory(
    zip_t*                       dest,
    std::filesystem::path const& directory,
    std::string const&           folder,
    PackExportStats&             stats
) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(directory, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() == ".tmp") continue;
        auto relative = std::filesystem::relative(it->path(), directory, ec);
        if (ec) break;
        auto relativeName = toUtf8(relative);
        std::ranges::replace(relativeName, '\\', '/');
        auto size = it->file_size(ec);
        if (ec) break;
        auto* buffer = static_cast<char*>(std::malloc(size == 0 ? 1 : static_cast<size_t>(size)));
        if (!buffer) continue;
        {
            std::ifstream input(it->path(), std::ios::binary);
            if (size > 0) input.read(buffer, static_cast<std::streamsize>(size));
            if (!input) {
                std::free(buffer);
                continue;
            }
        }
        auto entryName = std::string(ArchiveRoot) + folder + "/" + relativeName;
        if (addStoredBuffer(dest, entryName, buffer, size)) {
            stats.bytes += size;
            ++stats.entries;
        }
    }
    if (ec) getLogger().warn("Unable to read leftover pack directory {}: {}", directory, ec.message());
    ++stats.packs;
    return true;
}

} // namespace

PackExportStats writeReplayResourcePacks(zip_t* archive, std::filesystem::path const& recordDir) {
    PackExportStats stats;
    auto            root = recordDir / "resource_packs";
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return stats;

    auto index = readIndexFile(root);
    std::unordered_set<std::string> folders;
    for (auto const& pack : index["packs"]) {
        auto folder = pack.value("folder", std::string{});
        if (isFolderName(folder)) folders.insert(folder);
    }

    auto indexPath = root / "index.json";
    if (std::filesystem::is_regular_file(indexPath, ec)) {
        auto size = std::filesystem::file_size(indexPath, ec);
        if (!ec) {
            auto* buffer = static_cast<char*>(std::malloc(size == 0 ? 1 : static_cast<size_t>(size)));
            if (buffer) {
                std::ifstream input(indexPath, std::ios::binary);
                if (size > 0) input.read(buffer, static_cast<std::streamsize>(size));
                if (input) addStoredBuffer(archive, std::string(IndexEntry), buffer, size);
                else std::free(buffer);
            }
        }
    }

    for (auto const& folder : folders) {
        auto zipPath = root / (folder + ".zip");
        auto dirPath = root / folder;
        if (std::filesystem::is_regular_file(zipPath, ec)) {
            clonePackZip(archive, zipPath, folder, stats);
            continue;
        }
        if (std::filesystem::is_directory(dirPath, ec)) {
            clonePackDirectory(archive, dirPath, folder, stats);
        }
    }
    return stats;
}

PackEmbedWorker::PackEmbedWorker(std::filesystem::path recordDir)
    : mRecordDir(std::move(recordDir)),
      mThread([this](std::stop_token stop) { workerLoop(stop); }) {}

PackEmbedWorker::~PackEmbedWorker() {
    if (mThread.joinable()) cancel();
}

void PackEmbedWorker::submit(ServerPackSnapshot snapshot) {
    std::string signature;
    for (auto const& pack : snapshot.announced) {
        signature += std::format("{}_{}:{}:{};", pack.uuid, pack.version, pack.origin, pack.directory.empty());
    }
    {
        std::lock_guard lock(mMutex);
        if (mCancelled || mFinishing) return;
        // Snapshots repeat every five minutes; only a changed pack state needs another pass.
        if (signature == mLastSignature) return;
        mLastSignature = std::move(signature);
        mQueue.push_back(std::move(snapshot));
    }
    mCv.notify_one();
}

void PackEmbedWorker::finish() {
    {
        std::lock_guard lock(mMutex);
        mFinishing = true;
    }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

void PackEmbedWorker::cancel() {
    {
        std::lock_guard lock(mMutex);
        mCancelled = true;
        mQueue.clear();
    }
    mThread.request_stop();
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
    std::error_code ec;
    auto            root = mRecordDir / "resource_packs";
    if (!std::filesystem::is_directory(root, ec)) return;
    for (auto it = std::filesystem::directory_iterator(root, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        std::error_code removeError;
        if (it->path().extension() == ".part") std::filesystem::remove(it->path(), removeError);
    }
}

void PackEmbedWorker::workerLoop(std::stop_token stop) {
    while (true) {
        ServerPackSnapshot snapshot;
        {
            std::unique_lock lock(mMutex);
            mCv.wait(lock, [&] { return stop.stop_requested() || mCancelled || !mQueue.empty() || mFinishing; });
            if ((stop.stop_requested() || mCancelled) && mQueue.empty()) return;
            if (mQueue.empty()) {
                if (mFinishing) return;
                continue;
            }
            snapshot = std::move(mQueue.front());
            mQueue.erase(mQueue.begin());
        }
        (void)embedServerPacks(mRecordDir, snapshot, stop);
        if (stop.stop_requested()) return;
    }
}

} // namespace playback::io::packs
