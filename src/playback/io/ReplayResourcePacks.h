#pragma once

#include "playback/io/AsyncReplaySaver.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

struct zip;

class IResourcePackRepository;
struct PacksInfoData;

namespace playback::io::packs {

// A server resource pack; directory is empty while the client has not loaded it.
struct ServerPackSource {
    std::string           uuid;
    std::string           version;
    std::string           folder;
    std::filesystem::path directory;
    int                   origin{-1};
};

struct ServerPackSnapshot {
    std::vector<ServerPackSource>      announced;
    std::vector<std::filesystem::path> cacheDirectories;
};

// Client thread: resolves the packs announced by the latest recorded ResourcePacksInfo to the loaded packs.
[[nodiscard]] ServerPackSnapshot
collectServerPacks(std::vector<PlaybackSerializedGamePacket> const& configurationPackets);

struct EmbedJobResult {
    size_t present{};
    size_t zipped{};
    size_t failed{};
};

// Pack worker thread: zips each loaded pack into <recordDir>/resource_packs/<folder>.zip.
EmbedJobResult embedServerPacks(
    std::filesystem::path const& recordDir,
    ServerPackSnapshot const&    snapshot,
    std::stop_token              stop = {}
);

// Before export: zips announced packs that were never embedded from the recorded cache directories.
// Returns how many announced packs are still missing afterwards.
size_t completeServerPacks(std::filesystem::path const& recordDir);

struct PackExportStats {
    size_t   packs{};
    size_t   entries{};
    uint64_t bytes{};
};

// Export thread: clones each pack zip (or leftover pack directory) into resource_packs/<folder>/... of dest.
[[nodiscard]] PackExportStats writeReplayResourcePacks(struct zip* archive, std::filesystem::path const& recordDir);

// Independent of AsyncReplaySaver so pack I/O cannot stall chunk writes.
class PackEmbedWorker {
public:
    explicit PackEmbedWorker(std::filesystem::path recordDir);
    ~PackEmbedWorker();

    PackEmbedWorker(PackEmbedWorker const&)            = delete;
    PackEmbedWorker& operator=(PackEmbedWorker const&) = delete;
    PackEmbedWorker(PackEmbedWorker&&)                 = delete;
    PackEmbedWorker& operator=(PackEmbedWorker&&)      = delete;

    void submit(ServerPackSnapshot snapshot);

    // Runs the queued jobs to completion, then stops the thread.
    void finish();

    // Drops queued jobs, stops the running one and removes its partial zip.
    void cancel();

private:
    void workerLoop(std::stop_token stop);

    std::filesystem::path           mRecordDir;
    std::mutex                      mMutex;
    std::condition_variable         mCv;
    std::vector<ServerPackSnapshot> mQueue;
    std::string                     mLastSignature;
    bool                            mFinishing{false};
    bool                            mCancelled{false};
    std::jthread                    mThread;
};

// Client thread, after addCachedResourcePacks: mounts the embedded packs, plus local cache copies of announced packs
// the replay lacks, as the only cache pack source. Returns nullopt when the replay embeds no packs.
[[nodiscard]] std::optional<std::filesystem::path> mountReplayPacks(
    IResourcePackRepository&     repository,
    PacksInfoData const&         info,
    std::filesystem::path const& replayFile
);

// Keeps the extraction in the cache for the next load of the same replay, or deletes it.
void removeMountedPacks(std::filesystem::path const& mountDir);

} // namespace playback::io::packs
