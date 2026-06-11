#pragma once

#include "ll/api/base/StdInt.h"
#include "playback/Playback.h"

#include "mc/deps/core/utility/BinaryStream.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace playback::functions {

static constexpr int32_t MAGIC_NUMBER = 0x4C4C5042; // "LLPB" refer levilamina playback
static constexpr int32_t FILE_VERSION = 1;

class Action;
class ReplaySession;

class ReplayWriter {
private:
    enum State { STATE_EMPTY, STATE_WRITING_SNAPSHOT, STATE_WRITING_DATA };

    State mState = STATE_EMPTY;

    std::string  mBuffer;
    BinaryStream mStream{mBuffer};

    int32_t mSnapshotSizePos = -1;
    int32_t mActionSizePos   = -1;

    Action* mWritingAction = nullptr;

    std::unordered_map<std::string, int32_t> mActionNameToId;

public:
    void writeHeader();

    void startSnapshot();

    void endSnapshot();

    void startAndFinishAction(Action& action);

    void startAction(Action& action);

    void finishAction(Action& action);

private:
    ReplayWriter() = default;

public:
    [[nodiscard]] static ReplayWriter& getInstance() {
        static ReplayWriter instance;
        return instance;
    }
};

class ReplayReader {
private:
    std::string  mBuffer;
    BinaryStream mStream{mBuffer};
    int32_t      mVersion        = 0;
    int32_t      mSnapshotSize   = 0;
    uint64       mSnapshotOffset = 0;
    uint64       mActionOffset   = 0;

    std::string mLastActionName;

    std::unordered_map<int32_t, Action*> mActionMap;

public:
    explicit ReplayReader(std::string_view data);

    ReplayReader(ReplayReader const&)            = delete;
    ReplayReader& operator=(ReplayReader const&) = delete;

    void resetToStart() { mStream.mReadPointer = mActionOffset; };

    void handleSnapshot(ReplaySession& replaySession);

    bool handleNextAction(ReplaySession& replaySession);

private:
    ~ReplayReader() = default;
};

class AsyncReplaySaver {
public:
    using WriteTask = std::function<void(ReplayWriter&)>;

private:
    std::filesystem::path mRecordPath = Playback::getInstance().getSelf().getDataDir() / "record/temp";

    std::vector<WriteTask>  mQueue;
    std::mutex              mQueueMutex;
    std::condition_variable mCondition;
    std::thread             mWorkerThread;
    std::atomic<bool>       mRunning{false};
    std::atomic<bool>       mFinished{false};

private:
    void workerLoop();

public:
    AsyncReplaySaver();
    ~AsyncReplaySaver();

    AsyncReplaySaver(AsyncReplaySaver const&)            = delete;
    AsyncReplaySaver& operator=(AsyncReplaySaver const&) = delete;

    void submit(WriteTask task);

    std::filesystem::path finish();

    void cancel();

    [[nodiscard]] bool isRunning() const { return mRunning; }

    void writeGamePackets();

    void writeChunkCacheFile();

    void writeReplayChunk();

public:
    [[nodiscard]] static AsyncReplaySaver& getInstance() {
        static AsyncReplaySaver instance;
        return instance;
    }
};

} // namespace playback::functions
