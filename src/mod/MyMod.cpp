#include "mod/MyMod.h"

#include "Config.h"

#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/Config.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"

#include "ll/api/mod/RegisterHelper.h"

#include "replay/Record/Recorder.h"
#include "replay/playback/ReplayReader.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace replay {
namespace {

using Clock = std::chrono::system_clock;

ll::event::ListenerPtr gCommandRegisterListener{};
std::unique_ptr<record::Recorder> gRecorder{};
std::optional<record::ReplayArchive> gLastArchive{};

[[nodiscard]] UnixMillis nowUnixMillis() {
    return static_cast<UnixMillis>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()
    );
}

[[nodiscard]] std::vector<std::byte> toBytes(std::string_view text) {
    std::vector<std::byte> payload;
    payload.reserve(text.size());
    for (const unsigned char ch : text) {
        payload.push_back(static_cast<std::byte>(ch));
    }
    return payload;
}

} // namespace

MyMod& MyMod::getInstance() {
    static MyMod instance;
    return instance;
}

bool MyMod::load() {
    getSelf().getLogger().debug("Loading...");
    // Code for loading the mod goes here.

    // load Config
    const auto& configFilePath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configFilePath)) {
        getSelf().getLogger().warn("Cannot load configurations from {}", configFilePath);
        getSelf().getLogger().info("Saving default configurations");

        if (!ll::config::saveConfig(config, configFilePath)) {
            getSelf().getLogger().error("Cannot save default configurations to {}", configFilePath);
        }
    }

    return true;
}

bool MyMod::enable() {
    getSelf().getLogger().debug("Enabling...");
    registerCommands();
    return true;
}

bool MyMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    if (gCommandRegisterListener) {
        ll::event::EventBus::getInstance().removeListener<ll::event::command::ServerCommandRegisterEvent>(
            gCommandRegisterListener
        );
        gCommandRegisterListener.reset();
    }
    return true;
}

void MyMod::registerCommands() {
    if (gCommandRegisterListener) {
        return;
    }

    gCommandRegisterListener = ll::event::Listener<ll::event::command::ServerCommandRegisterEvent>::create(
        [this](ll::event::command::ServerCommandRegisterEvent&) {
            auto& registrar = ll::command::CommandRegistrar::getServerInstance();
            auto& command = registrar.getOrCreateCommand("replayrec", "Minimal replay recorder command");

            command.overload()
                .text("start")
                .execute([this](CommandOrigin const&, CommandOutput& output) {
                    if (!startMinimalRecording()) {
                        output.error("Replay recorder is already running");
                        return;
                    }
                    output.success("Replay recorder started");
                });

            command.overload()
                .text("stop")
                .execute([this](CommandOrigin const&, CommandOutput& output) {
                    if (!stopMinimalRecording()) {
                        output.error("Replay recorder is not running");
                        return;
                    }

                    const auto chunkCount = gLastArchive.has_value() ? gLastArchive->session.chunkCount() : 0;
                    playback::ReplayReader reader{
                        gLastArchive->session,
                        gLastArchive->payloadBlob,
                    };

                    if (!reader.buildTimeline()) {
                        output.error("Replay decode failed");
                        return;
                    }

                    output.success(
                        "Replay recorder stopped. chunks={}, frames={}",
                        static_cast<unsigned long long>(chunkCount),
                        static_cast<unsigned long long>(reader.frameCount())
                    );
                });
        }
    );

    ll::event::EventBus::getInstance().addListener(
        gCommandRegisterListener,
        ll::event::getEventId<ll::event::command::ServerCommandRegisterEvent>
    );
}

bool MyMod::startMinimalRecording() {
    if (gRecorder != nullptr && gRecorder->isRecording()) {
        return false;
    }

    record::RecorderConfig recorderConfig{};
    recorderConfig.compression = CompressionKind::None;
    recorderConfig.allowSparseChunk = false;

    ReplayMetadata metadata{};
    metadata.replayId = "demo-minimal";
    metadata.displayName = "Flashback Minimal Demo";
    metadata.gameVersion = "bedrock";
    metadata.modVersion = "dev";
    metadata.levelName = "unknown";
    metadata.dimensionId = "minecraft:overworld";
    metadata.recorderName = "server";
    metadata.createdAt = nowUnixMillis();
    metadata.startedAt = metadata.createdAt;
    metadata.snapshotIntervalTicks = 20 * 5;
    metadata.chunkDurationTicks = 20 * 30;

    gRecorder = std::make_unique<record::Recorder>(recorderConfig);
    gRecorder->begin(std::move(metadata));

    const auto snapshot = toBytes("snapshot:init-world-state");
    const auto actionA = toBytes("action:place_block");
    const auto actionB = toBytes("action:destroy_block");

    const auto startedAt = nowUnixMillis();
    gRecorder->pushSnapshot(0, startedAt, snapshot);
    gRecorder->pushAction(5, startedAt + 250, actionA);
    gRecorder->pushAction(10, startedAt + 500, actionB);
    return true;
}

bool MyMod::stopMinimalRecording() {
    if (gRecorder == nullptr || !gRecorder->isRecording()) {
        return false;
    }

    gLastArchive = gRecorder->finalizeArchive(nowUnixMillis());
    getSelf().getLogger().info(
        "Replay archive finalized: chunks={}, bytes={}",
        gLastArchive->session.chunkCount(),
        gLastArchive->session.metadata.totalBytes
    );
    return true;
}

} // namespace replay

LL_REGISTER_MOD(replay::MyMod, replay::MyMod::getInstance());
