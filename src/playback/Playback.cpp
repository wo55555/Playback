#include "Playback.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/command/Command.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/functions/tick/ClientTickHooks.h"
#include "playback/ui/MainMenuHooks.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/io/LogLevel.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/world/level/Level.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace playback {

struct Playback::Impl {
    config::Config                   mConfig;
    std::set<ll::event::ListenerPtr> mEventListeners;
    std::atomic<PlaybackMode>        mMode{PlaybackMode::Unknown};
    std::string                      mLevelId;
};

Playback::Playback() : impl(std::make_unique<Impl>()), mSelf(*ll::mod::NativeMod::current()) {}
Playback::~Playback() = default;

Playback& Playback::getInstance() {
    static Playback instance;
    return instance;
}

config::Config& Playback::getConfig() { return impl->mConfig; }

std::set<ll::event::ListenerPtr>& Playback::getEventListeners() { return impl->mEventListeners; }

void Playback::setupCommands() {
    auto& commandConfig = this->getConfig().command;

    command::registerPlaybackCommand();
    command::registerRecordCommand(commandConfig.record);
    command::registerReplayCommand(commandConfig.replay);
}

void Playback::registerActions() {
    auto& registry = functions::ActionRegistry::getInstance();

    registry.registerAction(std::make_unique<functions::ActionNextTick>());
    registry.registerAction(std::make_unique<functions::ActionLevelChunkCached>());
}

void Playback::unhook() {
    functions::hookClientTick(false);
    functions::hookNetwork(false);
    ui::hookMainMenu(false);
    getEventListeners().clear();
}

bool Playback::refreshMode() {
    auto level = ll::service::getMultiPlayerLevel();
    if (!level) {
        if (impl->mMode.load() != PlaybackMode::Unknown) {
            functions::Recorder::getInstance().clearChunkCache();
            impl->mLevelId.clear();
            impl->mMode.store(PlaybackMode::Unknown);
        }
        return false;
    }

    refreshMode(level.value());
    return impl->mMode.load() != PlaybackMode::Unknown;
}

void Playback::refreshMode(Level& level) {
    auto const& levelId = level.getLevelId();
    if (levelId.empty()) return;

    const auto replayPath = utils::PathUtils::getReplaysDir() / (levelId + ".playback");
    auto       mode       = std::filesystem::exists(replayPath) ? PlaybackMode::Replay : PlaybackMode::Record;

    if (impl->mLevelId != levelId) {
        if (!impl->mLevelId.empty() || mode == PlaybackMode::Replay) {
            functions::Recorder::getInstance().clearChunkCache();
        }
        impl->mLevelId = levelId;
    }

    impl->mMode.store(mode);
}

PlaybackMode Playback::getMode() const { return impl->mMode.load(); }

bool Playback::isReplayMode() const { return impl->mMode.load() == PlaybackMode::Replay; }

void configurationLog() {
    auto& logger = Playback::getInstance().getSelf().getLogger();
#ifdef DEBUG
    logger.setLevel(ll::io::LogLevel::Debug);
#endif
}

bool Playback::load() {
    configurationLog();

    const auto& logger = getSelf().getLogger();
    logger.debug("Loading...");

    ui::hookMainMenu(true);

    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>([this](auto&&) {
            setupCommands();
            registerActions();
            functions::hookNetwork(true);
            functions::hookClientTick(true);
        })
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientStartJoinLevelEvent>([this](auto&&) {
            functions::Recorder::getInstance().clearChunkCache();
            impl->mLevelId.clear();
            impl->mMode.store(PlaybackMode::Unknown);
        })
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientJoinLevelEvent>(
            [this](ll::event::ClientJoinLevelEvent& event) { refreshMode(event.player().getLevel()); }
        )
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([this](auto&&) {
            functions::Recorder::getInstance().clearChunkCache();
            impl->mLevelId.clear();
            impl->mMode.store(PlaybackMode::Unknown);
        })
    );
    return true;
}

bool Playback::enable() {
    const auto& logger = getSelf().getLogger();
    logger.debug("Enabling...");

    return true;
}

bool Playback::disable() {
    const auto& logger = getSelf().getLogger();
    logger.debug("Disabling...");
    return true;
}

} // namespace playback

LL_REGISTER_MOD(playback::Playback, playback::Playback::getInstance());
