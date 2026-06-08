#pragma once

#include "playback/functions/record/Recorder.h"

#include <filesystem>

class Minecraft;
class PacketSender;
class Level;

namespace playback::functions {

class ReplaySession {
private:
    static constexpr int CHUNK_LENGTH_SECONDS = 5 * 60;

    std::filesystem::path mReplayFilePath;
    std::filesystem::path mTempWorldPath;

    bool mActive     = false;
    bool mIsPaused   = false;
    bool mWorldReady = false;

    PlaybackMeta mMeta;

    Minecraft*    mClientMinecraft = nullptr;
    Minecraft*    mServerMinecraft = nullptr;
    PacketSender* mPacketSender    = nullptr;

private:
    bool init(std::filesystem::path filePath);

    // bool createTemporaryWorld();

    void onWorldReady();

    void applyInitialSnapshot();

    void cleanupTempWorld();

public:
    bool start(std::filesystem::path filePath);
    void stop();

    void tick();

    static void tryAutoStart(Level& level);

    void handleNextTick();
    void handleSnapShot();

private:
    ReplaySession() = default;

public:
    [[nodiscard]] static ReplaySession& getInstance() {
        static ReplaySession instance;
        return instance;
    }
};

} // namespace playback::functions
