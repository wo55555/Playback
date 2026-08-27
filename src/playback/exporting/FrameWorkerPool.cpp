#include "FrameWorkerPool.h"

#include <algorithm>

namespace playback::exporting {

namespace {

constexpr uint32_t MaxFrameWorkers = 8;
constexpr uint32_t MinRowsPerChunk = 16;

uint32_t resolveWorkerCount() {
    uint32_t const hardware = std::max(1u, std::thread::hardware_concurrency());
    return std::min(MaxFrameWorkers, std::max(1u, hardware - 1));
}

} // namespace

FrameWorkerPool::FrameWorkerPool(uint32_t workers) {
    if (workers <= 1) return;
    mThreads.reserve(workers - 1);
    for (uint32_t worker = 1; worker < workers; ++worker) {
        mThreads.emplace_back([this] { workerLoop(); });
    }
}

FrameWorkerPool::~FrameWorkerPool() {
    {
        std::scoped_lock lock(mMutex);
        mStopping = true;
    }
    mWake.notify_all();
    for (auto& thread : mThreads) {
        if (thread.joinable()) thread.join();
    }
}

void FrameWorkerPool::workerLoop() {
    uint64_t seen = 0;
    for (;;) {
        std::function<void(uint32_t, uint32_t)> const* body{};
        {
            std::unique_lock lock(mMutex);
            mWake.wait(lock, [&] { return mStopping || (mBody && mGeneration != seen); });
            if (mStopping) return;
            seen = mGeneration;
            body = mBody;
            ++mActive;
        }

        for (;;) {
            uint32_t first = 0;
            uint32_t last  = 0;
            {
                std::scoped_lock lock(mMutex);
                if (mNextChunk >= mRows) break;
                first      = mNextChunk;
                last       = std::min(mRows, first + mChunk);
                mNextChunk = last;
            }
            (*body)(first, last);
        }

        {
            std::scoped_lock lock(mMutex);
            --mActive;
            if (mActive == 0) mDone.notify_all();
        }
    }
}

void FrameWorkerPool::runRows(uint32_t rows, std::function<void(uint32_t, uint32_t)> const& body) {
    if (rows == 0) return;
    if (mThreads.empty()) {
        body(0, rows);
        return;
    }

    uint32_t const chunk = std::max(MinRowsPerChunk, (rows + workerCount() - 1) / workerCount());
    if (chunk >= rows) {
        body(0, rows);
        return;
    }

    {
        std::scoped_lock lock(mMutex);
        mBody      = &body;
        mRows      = rows;
        mChunk     = chunk;
        mNextChunk = 0;
        ++mGeneration;
    }
    mWake.notify_all();

    // The calling thread takes chunks too, so it never idles while the pool drains the frame.
    for (;;) {
        uint32_t first = 0;
        uint32_t last  = 0;
        {
            std::scoped_lock lock(mMutex);
            if (mNextChunk >= mRows) break;
            first      = mNextChunk;
            last       = std::min(mRows, first + mChunk);
            mNextChunk = last;
        }
        body(first, last);
    }

    std::unique_lock lock(mMutex);
    mDone.wait(lock, [&] { return mActive == 0; });
    mBody = nullptr;
}

FrameWorkerPool& frameWorkerPool() {
    static FrameWorkerPool pool(resolveWorkerCount());
    return pool;
}

} // namespace playback::exporting
