#include "Recorder.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/io/AsyncReplaySaver.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/storage/LevelData.h"

#include "nlohmann/json.hpp"
#include "nlohmann/json_fwd.hpp"

#include <uuid.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

std::string sanitizeFileName(std::string name) {
    for (auto& ch : name) {
        auto byte = static_cast<unsigned char>(ch);
        if (byte < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|'
            || ch == '?' || ch == '*') {
            ch = '_';
        }
    }

    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }

    return name.empty() ? "replay" : name;
}

std::string currentReplayTimestampName() {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%dT%H-%M-%S");
    return stream.str();
}

std::string uuidReplayName() {
    static std::random_device randomDevice;
    static std::mt19937       generator(randomDevice());

    auto id = uuids::uuid_random_generator(generator)();
    return uuids::to_string(id);
}

std::string findAvailableReplayName(std::filesystem::path const& replayDir, std::string baseName) {
    constexpr std::string_view extension = ".zip";

    baseName = sanitizeFileName(std::move(baseName));

    for (int index = 0; index < 10000; ++index) {
        std::string filename = baseName;
        if (index > 0) {
            filename += " (" + std::to_string(index) + ")";
        }
        filename += extension;

        std::error_code ec;
        bool            exists = std::filesystem::exists(replayDir / filename, ec);
        if (ec) {
            getLogger().error("Error while trying to determine replay filename: {}", ec.message());
            break;
        }
        if (!exists) {
            return filename;
        }
    }

    return uuidReplayName() + std::string(extension);
}

nlohmann::ordered_json metaToJson(PlaybackMeta const& meta) {
    auto chunks = nlohmann::ordered_json::object();
    for (auto const& [chunkName, chunkMeta] : meta.chunks) {
        chunks[chunkName] = metaToJson(chunkMeta);
    }

    return nlohmann::ordered_json{
        {"name",       meta.name        },
        {"worldName",  meta.worldName   },
        {"duration",   meta.duration    },
        {"totalTicks", meta.totalTicks  },
        {"chunks",     std::move(chunks)}
    };
}

PlaybackMeta metaFromJson(nlohmann::ordered_json const& json) {
    PlaybackMeta meta;
    meta.name       = json.value("name", meta.name);
    meta.worldName  = json.value("worldName", meta.worldName);
    meta.duration   = json.value("duration", meta.duration);
    meta.totalTicks = json.value("totalTicks", meta.totalTicks);

    auto chunksIt = json.find("chunks");
    if (chunksIt == json.end() || chunksIt->is_null()) {
        return meta;
    }

    if (!chunksIt->is_object()) {
        throw std::invalid_argument("Playback metadata chunks must be an object");
    }

    for (auto it = chunksIt->begin(); it != chunksIt->end(); ++it) {
        meta.chunks.insert_or_assign(it.key(), metaFromJson(it.value()));
    }
    return meta;
}

} // namespace

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto j = nlohmann::ordered_json::parse(json);
    return metaFromJson(j);
}

std::string PlaybackMeta::toJson() const { return metaToJson(*this).dump(); }

Recorder::Recorder() {
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }
}

void Recorder::start() {
    if (mState.load() == State::Paused) {
        mFinishedPausing = true;
        mState           = State::Recording;
        getLogger().debug("Resume recording");
        return;
    }

    // TODO: approve the replaymode logic
    auto& playback = playback::Playback::getInstance();
    if (playback.isReplayMode()) {
        mState = State::Idle;
        getLogger().debug("Skip recording because current save is a replay save");
        return;
    }

    if (mState.load() != State::Idle) {
        getLogger().debug("Recorder is already active");
        return;
    }

    resetStateForNewRecording();

    hookNetwork(true);

    mState = State::Recording;

    const auto& time =
        ll::service::getMultiPlayerLevel()
            .transform([](auto& level) { return ll::chrono::GameTickClock::fromTick(level.getCurrentTick()); })
            .value_or(ll::chrono::GameTickClock::time_point::min());
    getLogger().debug("current game tick={}", time.time_since_epoch());
}

void Recorder::pause() {
    if (mState.load() != State::Recording) return;

    mWasPaused = true;
    mState     = State::Paused;
}

void Recorder::stop() {
    State state = mState.exchange(State::Closing);
    if (state == State::Idle || state == State::Closing) {
        getLogger().debug("Recorder is not active");
        mState = State::Idle;
        return;
    }

    endTick(true);

    if (!mAsyncReplaySaver) {
        getLogger().error("Failed to stop recording because replay saver is not initialized");
        mState = State::Idle;
        return;
    }

    auto replayPath = mAsyncReplaySaver->finish();
    mAsyncReplaySaver.reset();
    mState = State::Idle;

    auto            replayDir = utils::PathUtils::getReplaysDir();
    std::error_code ec;
    std::filesystem::create_directories(replayDir, ec);
    if (ec) {
        getLogger().error("Error while trying to create replay folder: {}", ec.message());
    }

    auto outputPath = replayDir / findAvailableReplayName(replayDir, currentReplayTimestampName());
    if (!ReplayExporter::exportReplay(replayPath, outputPath, "")) {
        getLogger().error("Failed to save replay data after recording stopped");
        return;
    }
}

bool Recorder::readyToWrite() const {
    return mState.load() == State::Recording && !mNeedsInitialSnapshot.load() && !mWasPaused.load();
}

void Recorder::resetStateForNewRecording() {
    mAsyncReplaySaver = std::make_unique<AsyncReplaySaver>();

    {
        std::lock_guard             lock(mPendingPacketsMutex);
        std::queue<PacketWithPhase> empty;
        mPendingPackets.swap(empty);
    }

    mMetadata.chunks.clear();
    if (auto level = ll::service::getMultiPlayerLevel()) {
        mMetadata.worldName = level->getLevelData().mLevelName;
    }

    mChunkIndex           = 0;
    mTicksInCurrentChunk  = 0;
    mWrittenTicks         = 0;
    mHasOpenChunk         = false;
    mOpenChunkHasData     = false;
    mFinishedPausing      = false;
    mNeedsInitialSnapshot = true;
    mWasPaused            = false;
}

void Recorder::endTick(bool close) {
    const auto state = mState.load();
    if (state != State::Recording && state != State::Closing) return;

    if (mFinishedPausing) {
        if (mHasOpenChunk) {
            writeTickBoundary();
            finishCurrentChunk(false);
        }
        mFinishedPausing      = false;
        mWasPaused            = false;
        mNeedsInitialSnapshot = true;
    }

    writeInitialSnapshotIfNeeded();
    flushPendingPackets();

    bool wroteNewTick = false;
    if (!close) {
        writeTickBoundary();
        wroteNewTick = true;
    }

    if (close || mTicksInCurrentChunk >= RECORD_CHUNK_TICKS) {
        if (mTicksInCurrentChunk == 0 || !wroteNewTick) {
            writeTickBoundary();
        }
        finishCurrentChunk(close);
        if (!close) {
            writeInitialSnapshotIfNeeded();
        }
    }
}

void Recorder::recordGamePacket(std::shared_ptr<Packet> packet) {
    if (!packet) return;
}

void Recorder::cacheChunkPacket(LevelChunkPacket const& packet) {
    std::shared_ptr<Packet> packetToRecord;
    {
        auto packetCopy = std::make_shared<LevelChunkPacket>(packet);

        std::lock_guard lock(mChunkCacheMutex);
        mChunkCache[packetCopy->mPos] = packetCopy;
        packetToRecord                = packetCopy;
    }

    if (!packetToRecord) return;

    if (!readyToWrite()) return;

    {
        std::lock_guard lock(mPendingPacketsMutex);
        mPendingPackets.push(PacketWithPhase{std::move(packetToRecord), PacketPhase::Game});
    }
}

void Recorder::clearChunkCache() {
    {
        std::lock_guard lock(mChunkCacheMutex);
        mChunkCache.clear();
    }
    {
        std::lock_guard             lock(mPendingPacketsMutex);
        std::queue<PacketWithPhase> empty;
        mPendingPackets.swap(empty);
    }
}

void Recorder::writeSnapshot() {
    if (!mAsyncReplaySaver) return;

    mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.startSnapshot(); });

    std::vector<std::shared_ptr<Packet>> gamePackets;

    writeChunkDataSnapshot(gamePackets);

    mAsyncReplaySaver->writeGamePackets(std::move(gamePackets));

    mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.endSnapshot(); });
}

void Recorder::writeChunkDataSnapshot(std::vector<std::shared_ptr<Packet>>& gamePackets) {
    const auto& clientInstance = ll::service::getClientInstance();
    if (!clientInstance) return;

    const auto* localPlayer = clientInstance->getLocalPlayer();
    if (!localPlayer) return;

    const auto localPosition = localPlayer->getPosition();
    const auto localChunkX   = static_cast<int>(std::floor(localPosition.x / 16.0f));
    const auto localChunkZ   = static_cast<int>(std::floor(localPosition.z / 16.0f));

    std::lock_guard lock(mChunkCacheMutex);

    std::vector<std::pair<int, std::shared_ptr<LevelChunkPacket>>> sorted;
    sorted.reserve(mChunkCache.size());

    for (const auto& [pos, chunkPtr] : mChunkCache) {
        const auto dx = pos.x - localChunkX;
        const auto dz = pos.z - localChunkZ;
        sorted.emplace_back(dx * dx + dz * dz, chunkPtr);
    }

    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    for (auto& [_, chunkPtr] : sorted) {
        gamePackets.emplace_back(chunkPtr);
    }
}

void Recorder::writeInitialSnapshotIfNeeded() {
    if (!mNeedsInitialSnapshot.exchange(false)) return;

    writeSnapshot();
    mHasOpenChunk     = true;
    mOpenChunkHasData = true;
}

bool Recorder::flushPendingPackets() {
    std::queue<PacketWithPhase> pendingPackets;
    {
        std::lock_guard lock(mPendingPacketsMutex);
        mPendingPackets.swap(pendingPackets);
    }

    if (pendingPackets.empty()) return false;

    std::vector<std::shared_ptr<Packet>> gamePackets;
    while (!pendingPackets.empty()) {
        auto pendingPacket = std::move(pendingPackets.front());
        pendingPackets.pop();

        if (!pendingPacket.packet) continue;

        if (pendingPacket.phase == PacketPhase::Game) {
            gamePackets.emplace_back(std::move(pendingPacket.packet));
        }
    }

    if (gamePackets.empty()) return false;

    if (!mAsyncReplaySaver) return false;

    mAsyncReplaySaver->writeGamePackets(std::move(gamePackets));
    mHasOpenChunk     = true;
    mOpenChunkHasData = true;
    return true;
}

void Recorder::writeTickBoundary() {
    if (!mAsyncReplaySaver) return;

    mAsyncReplaySaver->submit([](ReplayWriter& writer) { writer.startAndFinishAction(ActionNextTick::getInstance()); });
    mHasOpenChunk     = true;
    mOpenChunkHasData = true;

    ++mTicksInCurrentChunk;
    ++mWrittenTicks;
}

void Recorder::finishCurrentChunk(bool close) {
    if (!mHasOpenChunk || !mOpenChunkHasData) return;

    std::string chunkName = "chunk_" + std::to_string(mChunkIndex) + ".bin";

    PlaybackMeta chunkMeta;
    chunkMeta.name      = chunkName;
    chunkMeta.worldName = mMetadata.worldName;
    chunkMeta.duration  = mTicksInCurrentChunk;
    mMetadata.chunks.insert_or_assign(chunkName, chunkMeta);
    mMetadata.totalTicks = mWrittenTicks;

    if (!mAsyncReplaySaver) return;

    mAsyncReplaySaver->writeReplayChunk(chunkName, mMetadata.toJson());

    ++mChunkIndex;
    mTicksInCurrentChunk = 0;
    mHasOpenChunk        = false;
    mOpenChunkHasData    = false;

    if (!close) {
        mNeedsInitialSnapshot = true;
    }
}

} // namespace playback::functions
