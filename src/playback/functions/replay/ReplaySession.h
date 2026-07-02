#pragma once

#include "playback/functions/record/Recorder.h"

#include <atomic>
#include <filesystem>

class Minecraft;
class PacketSender;
class Level;

namespace playback::functions {

class ReplaySession {
private:
    static constexpr int CHUNK_LENGTH_SECONDS = 5 * 60;

    int              mCurrentTick = 0;
    std::atomic<int> mTargetTick  = 0;

    bool mActive     = false;
    bool mIsPaused   = false;
    bool mWorldReady = false;

    std::filesystem::path mReplayFilePath;
    std::filesystem::path mTempWorldPath;

    PlaybackMeta mMeta;

    Minecraft*    mClientMinecraft = nullptr;
    Minecraft*    mServerMinecraft = nullptr;
    PacketSender* mPacketSender    = nullptr;

public:
    bool mIsProcessingSnapshot = false;

private:
    bool init(std::filesystem::path filePath);

    void onWorldReady();

    void applyInitialSnapshot();

    void cleanupTempWorld();

public:
    bool start(std::filesystem::path filePath);
    void stop();

    void tick();

    static void tryAutoStart(Level& level);

    void handleNextTick();

    bool sendRecordedTickPacket();

    void handleLevelChunkCached(int index);

private:
    ReplaySession() = default;

public:
    [[nodiscard]] static ReplaySession& getInstance() {
        static ReplaySession instance;
        return instance;
    }
};

} // namespace playback::functions
