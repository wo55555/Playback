#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

namespace playback::visuals {

// Export frames are tens of megabytes each, so freeing and reallocating one per frame dominates the capture path.
class FramePixelBufferPool {
public:
    [[nodiscard]] std::vector<std::byte> acquire(size_t bytes);
    void                                 release(std::vector<std::byte>&& buffer);
    void                                 clear();

private:
    std::mutex                          mMutex;
    std::vector<std::vector<std::byte>> mBuffers;
    size_t                              mPooledBytes{};
};

[[nodiscard]] FramePixelBufferPool& framePixelBufferPool();

} // namespace playback::visuals
