#include "Command.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/functions/replay/ReplaySession.h"
#include "playback/util/PathUtil.h"

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
    if (!config.enabled) {
        return;
    }

    auto& logger = getLogger();
    logger.debug("Start to register Replay commands");

    auto& replayCommand =
        ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(config.command, "回放状态控制");

    // replay start <filename>
    // TODO: 改成软枚举
    replayCommand.overload<ReplayStartParam>()
        .text("start")
        .required("filename")
        .execute([](CommandOrigin const&, CommandOutput& output, ReplayStartParam const& param) {
            auto replayPath = util::PathUtil::getReplaysDir() / param.filename;

            if (!functions::ReplaySession::getInstance().start(replayPath)) {
                // output.error("Failed to start replay session");
                return;
            }
        });
}

} // namespace playback::command
