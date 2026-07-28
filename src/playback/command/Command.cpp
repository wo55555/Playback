#include "Command.h"

#include "playback/Playback.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/i18n/I18n.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"

namespace playback::command {

void registerPlaybackCommand() {
    using ll::i18n_literals::operator""_tr;

    auto& command = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand(
        "playback",
        "playback.command.playback.description"_tr()
    );

    command.overload().text("version").execute([](CommandOrigin const&, CommandOutput& output) {
        auto const& version = Playback::getInstance().getSelf().getManifest().version;
        if (!version.has_value()) {
            output.error("playback.command.playback.versionUnavailable"_tr());
            return;
        }
        output.success("v{}", version->to_string());
    });
}

} // namespace playback::command
