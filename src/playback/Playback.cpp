#include "Playback.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/command/Command.h"
#include "playback/functions/action/Action.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/functions/tick/ClientTickHooks.h"
#include "playback/ui/MainMenuHooks.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientCancelJoinLevelEvent.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/io/LogLevel.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/multiplayer/MultiPlayerLevel.h"
#include "mc/world/level/Level.h"

#include <atomic>
#include <memory>
#include <string>

namespace playback {

struct Playback::Impl {
    config::Config                   mConfig;
    std::set<ll::event::ListenerPtr> mEventListeners;
    std::atomic<PlaybackMode>        mMode{PlaybackMode::Unknown};
    std::string                      mLevelId;
    bool                             mRuntimeInstalled{};
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
    registry.registerAction(std::make_unique<functions::ActionSubChunkCached>());
}

bool Playback::hook() {
    if (impl->mRuntimeInstalled) return true;

    ui::hookMainMenu(true);
    if (!functions::hookNetwork(true)) {
        ui::hookMainMenu(false);
        return false;
    }
    if (!functions::hookClientTick(true)) {
        if (!functions::hookNetwork(false)) {
            getSelf().getLogger().error("Unable to roll back replay network hooks after client tick hook failure");
        }
        ui::hookMainMenu(false);
        return false;
    }

    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>([this](auto&&) {
            setupCommands();
        })
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientStartJoinLevelEvent>([this](auto&&) {
            functions::ReplaySession::getInstance().onLevelStartJoin();
            functions::ChunkMutationBarrier::setActiveLevel(nullptr);
            impl->mLevelId.clear();
            impl->mMode.store(PlaybackMode::Unknown);
        })
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCancelJoinLevelEvent>([](auto&&) {
            functions::ReplaySession::getInstance().onLevelJoinCancelled();
        })
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientJoinLevelEvent>(
            [this](ll::event::ClientJoinLevelEvent& event) {
                functions::ChunkMutationBarrier::setActiveLevel(event.player().getLevel().asMultiPlayerLevel());
                functions::ReplaySession::getInstance().onLevelJoined(event.player());
                refreshMode(event.player().getLevel());
            }
        )
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientExitLevelEvent>([this](auto&&) {
            auto& replaySession = functions::ReplaySession::getInstance();
            replaySession.onLevelExit();
            auto& recorder = functions::Recorder::getInstance();
            recorder.stop();
            functions::ChunkMutationBarrier::setActiveLevel(nullptr);
            impl->mLevelId.clear();
            impl->mMode.store(PlaybackMode::Unknown);
        })
    );
    impl->mRuntimeInstalled = true;
    return true;
}

bool Playback::unhook() {
    if (!impl->mRuntimeInstalled) return true;
    if (!functions::hookClientTick(false)) return false;
    if (!functions::hookNetwork(false)) {
        bool tickRestored = functions::hookClientTick(true);
        getSelf().getLogger().error(
            "Unable to remove replay network hooks; client tick hook restoration={}",
            tickRestored
        );
        return false;
    }

    ui::hookMainMenu(false);
    getEventListeners().clear();
    impl->mLevelId.clear();
    impl->mMode.store(PlaybackMode::Unknown);
    impl->mRuntimeInstalled = false;
    return true;
}

bool Playback::refreshMode() {
    auto level = ll::service::getMultiPlayerLevel();
    if (!level) {
        if (impl->mMode.load() != PlaybackMode::Unknown) {
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

    auto mode = functions::ReplaySession::isReplayLevel(level) ? PlaybackMode::Replay : PlaybackMode::Record;

    if (impl->mLevelId != levelId) {
        impl->mLevelId = levelId;
    }

    impl->mMode.store(mode);
}

PlaybackMode Playback::getMode() const { return impl->mMode.load(); }

bool Playback::isReplayMode() const { return impl->mMode.load() == PlaybackMode::Replay; }

void configurationLog() {
#ifdef DEBUG
    auto& logger = Playback::getInstance().getSelf().getLogger();
    logger.setLevel(ll::io::LogLevel::Debug);
#endif
}

bool Playback::load() {
    configurationLog();

    const auto& logger = getSelf().getLogger();
    logger.debug("Loading...");

    registerActions();
    if (!hook()) {
        logger.error("Playback cannot load because its required network hooks are unavailable");
        return false;
    }
    return true;
}

bool Playback::enable() {
    const auto& logger = getSelf().getLogger();
    logger.debug("Enabling...");

    if (!hook()) {
        logger.error("Playback cannot enable because its required runtime hooks are unavailable");
        return false;
    }
    return true;
}

bool Playback::disable() {
    const auto& logger = getSelf().getLogger();
    logger.debug("Disabling...");

    auto& replaySession = functions::ReplaySession::getInstance();
    if (replaySession.isIsolatingReplayWorld() || replaySession.isReplayWorldCleanupPending()) {
        replaySession.stop();
        logger.error("Playback cannot disable until the replay world has finished closing and been removed");
        return false;
    }

    functions::Recorder::getInstance().stop();
    if (!unhook()) {
        logger.error("Playback cannot disable because its runtime hooks could not be removed safely");
        return false;
    }
    return true;
}

} // namespace playback

LL_REGISTER_MOD(playback::Playback, playback::Playback::getInstance());
