#include "Recorder.h"

#include "nlohmann/json_fwd.hpp"
#include "playback/Playback.h"
#include "playback/functions/io/AsyncReplaySaver.h"

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/reflection/Deserialization.h"
#include "ll/api/reflection/Serialization.h"
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

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto j = nlohmann::json::parse(json);
    return ll::reflection::deserialize_to<PlaybackMeta>(j).value();
}

std::string PlaybackMeta::toJson() const { return ll::reflection::serialize<nlohmann::json>(*this).value(); }

Recorder::Recorder() {
    auto&       levelData = ll::service::getMultiPlayerLevel()->getLevelData();
    std::string worldName = levelData.mLevelName;
    mMetadata.worldName   = worldName;
}

void Recorder::start() {
    const auto& time =
        ll::service::getMultiPlayerLevel()
            .transform([](auto& level) { return ll::chrono::GameTickClock::fromTick(level.getCurrentTick()); })
            .value_or(ll::chrono::GameTickClock::time_point::min());
    getLogger().debug("current game tick={}", time.time_since_epoch());
}

void Recorder::pause() {}

void Recorder::stop() {
    auto replayPath = mAsyncReplaySaver.finish();

    if (!ReplayExporter::saveReplayData(replayPath)) {
        getLogger().error("Failed to save replay data after recording stopped");
        return;
    }
}

void Recorder::cacheChunkPacket(LevelChunkPacket& packet) {
    std::lock_guard lock(mChunkCacheMutex);
    mChunkCache[packet.mPos] = std::make_shared<LevelChunkPacket>(packet);
}

void Recorder::writeSnapshot() {
    mAsyncReplaySaver.submit([](ReplayWriter& writer) { writer.startSnapshot(); });

    std::vector<std::unique_ptr<Packet>> gamePackets;

    writeChunkDataSnapshot(gamePackets);

    mAsyncReplaySaver.writeGamePackets(std::move(gamePackets));

    mAsyncReplaySaver.submit([](ReplayWriter& writer) { writer.endSnapshot(); });
}

void Recorder::writeChunkDataSnapshot(std::vector<std::unique_ptr<Packet>>& gamePackets) {
    const auto& clientInstance = ll::service::getClientInstance();
    const auto* localPlayer    = clientInstance->getLocalPlayer();
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
        gamePackets.emplace_back(std::make_unique<LevelChunkPacket>(*chunkPtr));
    }
}

} // namespace playback::functions
