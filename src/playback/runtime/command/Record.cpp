#include "Command.h"

#include "playback/Playback.h"
#include "playback/configuration/Config.h"
#include "playback/record/Recorder.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::runtime::command {

// Command parameters are reflected by name, which needs a type with linkage.
struct RecordRateParam {
    int rate;
};

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

    recordCommand.overload().text("rate").execute([](CommandOrigin const&, CommandOutput& output) {
        int const rate = Playback::getInstance().getConfig().record.localPlayerUpdatesPerSecond;
        output.success("playback.command.record.rate"_tr(rate));
    });

    recordCommand.overload<RecordRateParam>().text("rate").required("rate").execute(
        [](CommandOrigin const&, CommandOutput& output, RecordRateParam const& param) {
            int const rate = configuration::clampLocalPlayerUpdatesPerSecond(param.rate);
            Playback::getInstance().getConfig().record.localPlayerUpdatesPerSecond = rate;
            output.success("playback.command.record.rateSet"_tr(rate));
        }
    );
}

} // namespace playback::runtime::command
