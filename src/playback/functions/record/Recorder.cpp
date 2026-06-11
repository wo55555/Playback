#include "Recorder.h"

#include "playback/Playback.h"

#include "ll/api/chrono/GameChrono.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/packet/LevelChunkPacket.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/chunk/LevelChunk.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace playback::functions {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

PlaybackMeta PlaybackMeta::fromJson(std::string_view json) {
    auto         j = nlohmann::json::parse(json);
    PlaybackMeta meta;
    meta.worldName = j.value("worldName", "");
    return meta;
}

std::string PlaybackMeta::toJson() const {
    nlohmann::json j;
    j["worldName"] = worldName;
    return j.dump();
}

void Recorder::start() {
    const auto& time =
        ll::service::getMultiPlayerLevel()
            .transform([](auto& level) { return ll::chrono::GameTickClock::fromTick(level.getCurrentTick()); })
            .value_or(ll::chrono::GameTickClock::time_point::min());
    getLogger().debug("current game tick={}", time.time_since_epoch());
}

void Recorder::pause() {}

void Recorder::stop() {}

void Recorder::cacheChunkPacket(
    ::ChunkPos      pos,
    ::DimensionType dimId,
    uint64          subChunksCount,
    std::string&&   serializedChunk
) {
    std::lock_guard lock(mChunkCacheMutex);
    mChunkCache[pos] = {pos, dimId, subChunksCount, std::move(serializedChunk)};
}

void Recorder::writeChunkDataSnapshot(std::list<std::unique_ptr<Packet>>& gamePackets) {
    const auto& clientInstance = ll::service::getClientInstance();
    const auto* localPlayer    = clientInstance->getLocalPlayer();
    if (!localPlayer) return;

    const auto localPosition = localPlayer->getPosition();
    const auto localChunkX   = static_cast<int>(std::floor(localPosition.x / 16.0f));
    const auto localChunkZ   = static_cast<int>(std::floor(localPosition.z / 16.0f));

    // 从缓存中提取所有原生网络数据包，按距离排序
    std::lock_guard lock(mChunkCacheMutex);

    std::vector<std::pair<std::int64_t, ChunkPacketData>> sorted;
    sorted.reserve(mChunkCache.size());

    for (const auto& [pos, data] : mChunkCache) {
        const auto deltaX          = static_cast<std::int64_t>(pos.x - localChunkX);
        const auto deltaZ          = static_cast<std::int64_t>(pos.z - localChunkZ);
        const auto distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
        sorted.emplace_back(distanceSquared, data);
    }

    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    for (auto& [_, cacheData] : sorted) {
        auto packet             = std::make_unique<LevelChunkPacket>();
        packet->mPos            = cacheData.pos;
        packet->mDimensionId    = cacheData.dimId;
        packet->mSubChunksCount = cacheData.subChunksCount;

        // 使用缓存的原始网络二进制数据
        packet->mSerializedChunk = std::move(cacheData.serializedChunk);
        gamePackets.emplace_back(std::move(packet));
    }
}

} // namespace playback::functions
