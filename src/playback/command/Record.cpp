#include "Command.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/functions/record/Recorder.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::command {

namespace {

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

void registerRecordCommand(config::CommandConfigStruct& config) {
    if (!config.enabled) {
        return;
    }

    auto& logger = getLogger();
    logger.debug("Start to register Record commands");

    auto& recordCommand =
        ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(config.command, "控制录制状态");

    recordCommand.overload().text("start").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = functions::Recorder::getInstance();
        recorder.start();

        output.success("录制已开始");
    });

    recordCommand.overload().text("pause").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = functions::Recorder::getInstance();
        recorder.pause();

        output.success("录制暂停");
    });

    recordCommand.overload().text("stop").execute([](CommandOrigin const&, CommandOutput& output) {
        auto& recorder = functions::Recorder::getInstance();
        recorder.stop();

        output.success("录制结束");
    });

    playback::functions::hookNetwork(true);
}

} // namespace playback::command
