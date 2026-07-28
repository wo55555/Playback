#include "Command.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::command {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

void registerRecordCommand(config::CommandConfigStruct& config) {
    using ll::i18n_literals::operator""_tr;

    if (!config.enabled) {
        return;
    }

    auto& logger = getLogger();
    logger.debug("Start to register Record commands");

    auto& recordCommand = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        config.command,
        "playback.command.record.description"_tr()
    );

    recordCommand.overload().text("start").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = functions::Recorder::getInstance();
        recorder.start();

        auto& logger = getLogger();
        logger.debug("name={}", Playback::getInstance().getSelf().getName());

        output.success(ll::i18n::getInstance().get("playback.command.record.started", {}));
    });

    recordCommand.overload().text("pause").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = functions::Recorder::getInstance();
        recorder.pause();

        output.success(ll::i18n::getInstance().get("playback.command.record.paused", {}));
    });

    recordCommand.overload().text("stop").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = functions::Recorder::getInstance();
        recorder.stop();

        output.success(ll::i18n::getInstance().get("playback.command.record.stopped", {}));
    });
}

} // namespace playback::command
