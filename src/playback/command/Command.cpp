#include "Command.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::command {

void registerPlaybackCommand() {
    auto& command = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand("playback", "Playback Mod");

    command.overload().text("version").execute([](CommandOrigin const&, CommandOutput& output) {
        output.success("v0.0.0");
    });
}

} // namespace playback::command
