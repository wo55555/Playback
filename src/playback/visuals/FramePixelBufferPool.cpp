#include "FramePixelBufferPool.h"

#include <algorithm>
#include <utility>

namespace playback::visuals {

namespace {

constexpr size_t MaxPooledBuffers = 12;
constexpr size_t MaxPooledBytes   = 768ull * 1024 * 1024;

} // namespace

std::vector<std::byte> FramePixelBufferPool::acquire(size_t bytes) {
    std::vector<std::byte> buffer;
    {
        std::scoped_lock lock(mMutex);
        // Buffers keep their length while pooled, so the best fit avoids both a reallocation and a zero fill.
        auto best = mBuffers.end();
        for (auto it = mBuffers.begin(); it != mBuffers.end(); ++it) {
            if (it->capacity() < bytes) continue;
            if (best == mBuffers.end() || it->capacity() < best->capacity()) best = it;
        }
        if (best == mBuffers.end() && !mBuffers.empty()) {
            best = std::ranges::max_element(mBuffers, {}, [](auto const& candidate) { return candidate.capacity(); });
        }
        if (best != mBuffers.end()) {
            buffer        = std::move(*best);
            mPooledBytes -= std::min(mPooledBytes, buffer.capacity());
            mBuffers.erase(best);
        }
    }
    buffer.resize(bytes);
    return buffer;
}

void FramePixelBufferPool::release(std::vector<std::byte>&& buffer) {
    if (buffer.capacity() == 0) return;
    std::scoped_lock lock(mMutex);
    if (mBuffers.size() >= MaxPooledBuffers || mPooledBytes + buffer.capacity() > MaxPooledBytes) return;
    mPooledBytes += buffer.capacity();
    mBuffers.emplace_back(std::move(buffer));
}

void FramePixelBufferPool::clear() {
    std::vector<std::vector<std::byte>> buffers;
    {
        std::scoped_lock lock(mMutex);
        buffers = std::move(mBuffers);
        mBuffers.clear();
        mPooledBytes = 0;
    }
}

FramePixelBufferPool& framePixelBufferPool() {
    static FramePixelBufferPool pool;
    return pool;
}

} // namespace playback::visuals
