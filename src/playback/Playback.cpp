#include "Playback.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/command/Command.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/LogLevel.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

#include <memory>

namespace playback {

struct Playback::Impl {
    config::Config                   mConfig;
    std::set<ll::event::ListenerPtr> mEventListeners;
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

void Playback::unhook() {
    functions::hookNetwork(false);
    getEventListeners().clear();
}

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

    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>([this](auto&&) {
            setupCommands();
        })
    );
    getEventListeners().emplace(
        ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>(
            [](ll::event::ServerLevelTickEvent& ev) { functions::ReplaySession::tryAutoStart(ev.level()); }
        )
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
