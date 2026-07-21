#include "Command.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"

#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::command {

namespace {

struct ReplayStartParam {
    std::string filename;
};

auto& getLogger() { return playback::Playback::getInstance().getSelf().getLogger(); }

} // namespace

void registerReplayCommand(config::CommandConfigStruct& config) {
    auto& logger = getLogger();
    logger.debug("Start to register Replay commands");

    auto& replayCommand =
        ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(config.command, "回放状态控制");

    if (config.enabled) {
        // replay start <filename>
        // TODO: 改成软枚举
        replayCommand.overload<ReplayStartParam>()
            .text("start")
            .required("filename")
            .execute([](CommandOrigin const&, CommandOutput& output, ReplayStartParam const& param) {
                auto replayPath = utils::PathUtils::getReplaysDir() / param.filename;

                if (!functions::ReplaySession::getInstance().start(replayPath)) {
                    output.error("Failed to start replay session");
                    return;
                }
            });
    } else {
        logger.debug("Replay start command is disabled; registering playback controls only");
    }

    replayCommand.overload().text("play").execute([](CommandOrigin const&, CommandOutput& output) {
        if (!functions::ReplaySession::getInstance().setPaused(false)) {
            output.error("No replay session is active");
            return;
        }
        output.success("Replay playing");
    });

    replayCommand.overload().text("pause").execute([](CommandOrigin const&, CommandOutput& output) {
        if (!functions::ReplaySession::getInstance().setPaused(true)) {
            output.error("No replay session is active");
            return;
        }
        output.success("Replay paused");
    });
}

} // namespace playback::command
