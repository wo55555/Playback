#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace playback::exporting {

// Row-parallel helper for export pixel work; spawning threads per frame stalls the writer thread entirely.
class FrameWorkerPool {
public:
    explicit FrameWorkerPool(uint32_t workers);
    ~FrameWorkerPool();

    FrameWorkerPool(FrameWorkerPool const&)            = delete;
    FrameWorkerPool& operator=(FrameWorkerPool const&) = delete;

    // Splits [0, rows) across the pool and the calling thread, returning once every chunk has finished.
    void runRows(uint32_t rows, std::function<void(uint32_t, uint32_t)> const& body);

    [[nodiscard]] uint32_t workerCount() const noexcept { return static_cast<uint32_t>(mThreads.size()) + 1; }

private:
    void workerLoop();

    std::vector<std::thread>                       mThreads;
    std::mutex                                     mMutex;
    std::condition_variable                        mWake;
    std::condition_variable                        mDone;
    std::function<void(uint32_t, uint32_t)> const* mBody{};
    uint32_t                                       mRows{};
    uint32_t                                       mChunk{};
    uint32_t                                       mNextChunk{};
    uint32_t                                       mActive{};
    uint64_t                                       mGeneration{};
    bool                                           mStopping{};
};

[[nodiscard]] FrameWorkerPool& frameWorkerPool();

} // namespace playback::exporting
