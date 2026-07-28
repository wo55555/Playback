#pragma once

#include "playback/utils/PathUtils.h"

#include "mc/deps/core/utility/BinaryStream.h"

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

class Packet;

namespace playback::functions {

static constexpr int32_t MAGIC_NUMBER     = 0x4C4C5042; // "LLPB" refer levilamina playback
static constexpr int32_t CHUNK_CACHE_SIZE = 10000;

class Action;
class ReplaySession;
class CachedChunkPacket;

struct PlaybackSerializedGamePacket {
    int32_t     mPacketId;
    std::string mPayload;
};

class PlaybackBuffer : public BinaryStream {
public:
    using BinaryStream::BinaryStream;

    [[nodiscard]] uint64_t getWritePointer() const { return mBuffer.size(); }

    void writeAt(uint64_t pos, void const* data, size_t size) {
        if (pos + size > mBuffer.size()) {
            mBuffer.resize(pos + size);
        }
        std::memcpy(mBuffer.data() + pos, data, size);
    }

    template <std::integral T>
    void writeAt(uint64_t pos, T value) {
        writeAt(pos, &value, sizeof(T));
    }

    void clear() {
        mBuffer.clear();
        mReadPointer = 0;
    }
};

class ReplayWriter {
private:
    enum State { STATE_EMPTY, STATE_WRITING_SNAPSHOT, STATE_WRITING_DATA };

    State mState = STATE_EMPTY;

    int32_t mSnapshotSizePos = -1;
    int32_t mActionSizePos   = -1;

    Action* mWritingAction = nullptr;

    std::unordered_map<std::string, int32_t> mActionNameToId;

public:
    ReplayWriter()  = default;
    ~ReplayWriter() = default;

    PlaybackBuffer mStream;

public:
    void writeHeader();

    void startSnapshot();

    void endSnapshot();

    void startAndFinishAction(Action& action);

    void startAction(Action& action);

    void finishAction(Action& action);

    std::string popBuffer();
};

class ReplayReader {
private:
    std::string    mBuffer;
    PlaybackBuffer mStream{mBuffer};
    uint32_t       mSnapshotSize   = 0;
    uint64_t       mSnapshotOffset = 0;
    uint64_t       mActionsOffset  = 0;

    std::string mLastActionName;

    std::unordered_map<int32_t, Action*> mActionMap;

public:
    explicit ReplayReader(std::string_view data);
    ~ReplayReader() = default;

    ReplayReader(ReplayReader const&)            = delete;
    ReplayReader& operator=(ReplayReader const&) = delete;

    void resetToStart() { mStream.mReadPointer = mActionsOffset; };

    void handleSnapshot(ReplaySession& replaySession);

    bool handleNextAction(ReplaySession& replaySession);
};

class AsyncReplaySaver {
public:
    using WriteTask  = std::function<void(ReplayWriter&)>;
    using GamePacket = std::variant<std::shared_ptr<Packet>, PlaybackSerializedGamePacket>;

private:
    ReplayWriter mReplayWriter;

    std::filesystem::path mRecordPath;

    std::vector<WriteTask>     mQueue;
    mutable std::mutex         mQueueMutex;
    std::condition_variable    mCondition;
    std::thread                mWorkerThread;
    std::atomic<bool>          mRunning{false};
    std::atomic<bool>          mFinished{false};
    bool                       mCancelled = false;
    std::optional<std::string> mError;

    std::unordered_map<uint64_t, std::vector<CachedChunkPacket>> mCachedChunkPackets;
    PlaybackBuffer                                               mChunkCacheOutput;

    int mTotalWrittenChunkPackets = 0;
    int mCurrentChunkCacheIndex   = -1;
    int mWrittenChunkCacheFiles   = 0;

private:
    void workerLoop();

    void recordError(std::string error);

    void flushCurrentChunkCacheFile();

public:
    AsyncReplaySaver();
    ~AsyncReplaySaver();

    AsyncReplaySaver(AsyncReplaySaver const&)            = delete;
    AsyncReplaySaver& operator=(AsyncReplaySaver const&) = delete;

    bool submit(WriteTask task);

    std::filesystem::path finish();

    void cancel();

    [[nodiscard]] bool isRunning() const { return mRunning; }

    [[nodiscard]] bool hasError() const;

    [[nodiscard]] std::optional<std::string> getError() const;

    bool writeGamePackets(std::vector<GamePacket> packets);

    void writeChunkCacheFile(PlaybackBuffer const& chunkCacheOutput, int index);

    bool writeReplayChunk(std::string chunkName, std::string metadata);
};

} // namespace playback::functions
