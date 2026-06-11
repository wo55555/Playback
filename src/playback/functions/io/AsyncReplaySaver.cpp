#include "AsyncReplaySaver.h"

#include <filesystem>
#include <mutex>

namespace playback::functions {

AsyncReplaySaver::AsyncReplaySaver() {
    mRunning      = true;
    mFinished     = false;
    mWorkerThread = std::thread(&AsyncReplaySaver::workerLoop, this);
}

AsyncReplaySaver::~AsyncReplaySaver() {
    if (mRunning) {
        cancel();
    }
}

void AsyncReplaySaver::workerLoop() {
    auto& writer = ReplayWriter::getInstance();

    while (true) {
        WriteTask task;

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mCondition.wait(lock, [this] { return !mQueue.empty() || !mRunning; });

            if (!mRunning && mQueue.empty()) {
                break;
            }

            if (!mQueue.empty()) {
                task = std::move(mQueue.front());
                mQueue.erase(mQueue.begin());
            }
        }

        if (task) {
            task(writer);
        }
    }

    mFinished = true;
}

void AsyncReplaySaver::submit(WriteTask task) {
    if (!mRunning) return;

    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mQueue.push_back(std::move(task));
    }
    mCondition.notify_one();
}

std::filesystem::path AsyncReplaySaver::finish() {
    if (!mRunning) return mRecordPath;

    mRunning = false;
    mCondition.notify_all();

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }

    return mRecordPath;
}

void AsyncReplaySaver::cancel() {
    mRunning = false;

    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mQueue.clear();
    }
    mCondition.notify_all();

    if (mWorkerThread.joinable()) {
        mWorkerThread.join();
    }
}

void AsyncReplaySaver::writeGamePackets() {}

void AsyncReplaySaver::writeChunkCacheFile() {}

void AsyncReplaySaver::writeReplayChunk() {}

} // namespace playback::functions
