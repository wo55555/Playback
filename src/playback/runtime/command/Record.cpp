#include "Command.h"

#include "playback/configuration/Config.h"
#include "playback/record/Recorder.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::runtime::command {

void registerRecordCommand(configuration::CommandConfigStruct& config) {
    using namespace ll::i18n_literals;

    if (!config.enabled) {
        return;
    }

    auto& recordCommand = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        config.command,
        "playback.command.record.description"_tr()
    );

    recordCommand.overload().text("start").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = record::Recorder::getInstance();
        recorder.start();

        output.success("playback.command.record.started"_tr());
    });

    recordCommand.overload().text("pause").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = record::Recorder::getInstance();
        recorder.pause();

        output.success("playback.command.record.paused"_tr());
    });

    recordCommand.overload().text("stop").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = record::Recorder::getInstance();
        recorder.stop();

        output.success("playback.command.record.stopped"_tr());
    });
}

} // namespace playback::runtime::command
