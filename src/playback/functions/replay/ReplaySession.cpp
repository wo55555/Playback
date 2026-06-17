#include "ReplaySession.h"

#include "playback/Playback.h"
#include "playback/functions/action/Action.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/world/Minecraft.h"
#include "mc/world/level/Level.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace playback::functions {

bool ReplaySession::start(std::filesystem::path filePath) {
    if (mActive) return false;
    if (!init(std::move(filePath))) return false;

    return true;
}

void ReplaySession::stop() {
    mActive     = false;
    mWorldReady = false;
    mIsPaused   = false;

    cleanupTempWorld();
}

void ReplaySession::tick() {
    if (!mActive) return;

    if (!mWorldReady) {
        onWorldReady();
        return;
    }
}

void ReplaySession::tryAutoStart(Level& level) {
    auto& session = getInstance();
    if (session.mActive) return;

    std::string const& levelId = level.getLevelId();
    if (levelId.empty()) return;

    auto replayPath = utils::PathUtils::getReplaysDir() / (levelId + ".playback");
    if (!std::filesystem::exists(replayPath)) return;

    session.start(replayPath);
}

bool ReplaySession::init(std::filesystem::path filePath) {
    mReplayFilePath = std::move(filePath);

    // TODO: Read files
    // std::ifstream file(mReplayFilePath, std::ios::binary);
    // if (!file.is_open()) return false;

    // std::stringstream buffer;
    // buffer << file.rdbuf();
    // std::string zipData = buffer.str();
    // file.close();

    // std::string metadataJson;
    // std::vector<> chunkFiles;

    // TODO: Register actions

    // Create temporary world
    // if (!createTemporaryWorld()) return false;

    auto clientMinecraft = ll::service::getMinecraft(true);
    auto serverMinecraft = ll::service::getMinecraft(false);
    if (!clientMinecraft || !serverMinecraft) return false;
    mClientMinecraft = &clientMinecraft.value();
    mServerMinecraft = &serverMinecraft.value();

    auto listener =
        ll::event::Listener<ll::event::ServerLevelTickEvent>::create([this](ll::event::ServerLevelTickEvent&) {
            if (!mWorldReady) {
                mPacketSender = mServerMinecraft->getLevel()->getPacketSender();
            }
            tick();
        });
    ll::event::EventBus::getInstance().addListener<ll::event::ServerLevelTickEvent>(listener);

    mActive = true;
    return true;
}

void ReplaySession::onWorldReady() {
    applyInitialSnapshot();
    mWorldReady = true;
}

void ReplaySession::applyInitialSnapshot() {}

void ReplaySession::cleanupTempWorld() {
    if (std::filesystem::exists(mTempWorldPath)) {
        std::error_code ec;
        std::filesystem::remove_all(mTempWorldPath, ec);
    }
}

void ReplaySession::handleNextTick() {
    if (mIsProcessingSnapshot) {
        throw std::runtime_error("Can't go to next tick while processing snapshot");
    }
    // TODO: Flash pending entities

    mCurrentTick += 1;
}

void ReplaySession::handleLevelChunkCached(int index) {
    // TODO: Handle cached level chunk at the given index
}

} // namespace playback::functions
