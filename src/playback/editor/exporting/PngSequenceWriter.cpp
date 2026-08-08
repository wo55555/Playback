#include "PngSequenceWriter.h"

#include "FrameWriterUtils.h"
#include "playback/functions/render/ReplayThumbnail.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace playback::editor::exporting {

namespace {

[[nodiscard]] std::filesystem::path framePath(std::filesystem::path const& directory, uint64_t frameIndex) {
    std::ostringstream name;
    name << "frame_" << std::setfill('0') << std::setw(6) << frameIndex << ".png";
    return directory / name.str();
}

[[nodiscard]] std::filesystem::path temporaryFramePath(std::filesystem::path const& directory, uint64_t frameIndex) {
    auto path  = framePath(directory, frameIndex);
    path      += ".part";
    return path;
}

void removeIncompleteOutput(std::filesystem::path const& directory, bool ownsDirectory, uint64_t frameCount) {
    std::error_code ec;
    if (ownsDirectory) {
        std::filesystem::remove_all(directory, ec);
        return;
    }
    for (uint64_t index = 0; index <= frameCount; ++index) {
        std::filesystem::remove(framePath(directory, index), ec);
        std::filesystem::remove(temporaryFramePath(directory, index), ec);
    }
}

} // namespace

struct PngSequenceWriter::Impl {
    explicit Impl(uint32_t queueCapacity) : capacity(std::max<uint32_t>(1, queueCapacity)) {}

    ~Impl() {
        requestCancel();
        wait();
    }

    struct QueuedFrame {
        functions::render::CapturedFrame frame;
        uint64_t                         index{};
    };

    uint32_t                capacity;
    mutable std::mutex      mutex;
    std::condition_variable changed;
    std::deque<QueuedFrame> queue;
    std::filesystem::path   directory;
    FrameWriterState        state{FrameWriterState::Idle};
    bool                    ownsDirectory{};
    uint64_t                submitted{};
    uint64_t                written{};
    uint64_t                nextFrameIndex{};
    uint32_t                frameWidth{};
    uint32_t                frameHeight{};
    ExportError             error{ExportError::None};
    std::string             message;
    std::filesystem::path   latestFramePath;
    std::thread             worker;

    void requestCancel() {
        std::scoped_lock lock(mutex);
        if (state == FrameWriterState::Running || state == FrameWriterState::Finishing) {
            state = FrameWriterState::Cancelling;
            queue.clear();
            changed.notify_all();
        }
    }

    void wait() {
        if (worker.joinable()) worker.join();
    }

    void setFailureLocked(ExportError failure, std::string text) {
        if (error == ExportError::None) {
            error   = failure;
            message = std::move(text);
        }
        queue.clear();
        state = FrameWriterState::Cancelling;
    }

    void workerLoop() {
        std::vector<uint8_t> rgba;
        while (true) {
            QueuedFrame item;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    return !queue.empty() || state == FrameWriterState::Finishing
                        || state == FrameWriterState::Cancelling;
                });
                if (state == FrameWriterState::Cancelling) {
                    queue.clear();
                    break;
                }
                if (queue.empty()) {
                    state = FrameWriterState::Completed;
                    changed.notify_all();
                    return;
                }
                item = std::move(queue.front());
                queue.pop_front();
                changed.notify_all();
            }

            detail::copyPackedRgba(item.frame, rgba);
            auto const output  = framePath(directory, item.index);
            auto const partial = temporaryFramePath(directory, item.index);
            bool const encoded = functions::render::writeRgbaPng(
                partial,
                item.frame.width,
                item.frame.height,
                rgba.data(),
                item.frame.width * 4
            );

            bool committed = false;
            {
                std::scoped_lock lock(mutex);
                if (encoded && state != FrameWriterState::Cancelling) {
                    std::error_code ec;
                    std::filesystem::rename(partial, output, ec);
                    if (ec) {
                        setFailureLocked(ExportError::WriteFailed, "Unable to commit a PNG export frame");
                    } else {
                        ++written;
                        latestFramePath = output;
                        committed       = true;
                    }
                } else if (!encoded && state != FrameWriterState::Cancelling) {
                    setFailureLocked(ExportError::WriteFailed, "Unable to write a PNG export frame");
                }
                changed.notify_all();
                if (state == FrameWriterState::Cancelling || error != ExportError::None) break;
            }
            if (!committed) {
                std::error_code ec;
                std::filesystem::remove(partial, ec);
            }
        }

        std::filesystem::path cleanupDirectory;
        bool                  ownsDirectoryCopy{};
        uint64_t              cleanupFrameCount{};
        {
            std::scoped_lock lock(mutex);
            cleanupDirectory  = directory;
            ownsDirectoryCopy = ownsDirectory;
            cleanupFrameCount = nextFrameIndex;
            queue.clear();
        }
        removeIncompleteOutput(cleanupDirectory, ownsDirectoryCopy, cleanupFrameCount);

        {
            std::scoped_lock lock(mutex);
            ownsDirectory = false;
            latestFramePath.clear();
            state = error == ExportError::None ? FrameWriterState::Cancelled : FrameWriterState::Faulted;
            changed.notify_all();
        }
    }

    void resetForOpen() {
        std::scoped_lock lock(mutex);
        directory.clear();
        state         = FrameWriterState::Idle;
        ownsDirectory = false;
        submitted = written = nextFrameIndex = 0;
        frameWidth = frameHeight = 0;
        error                    = ExportError::None;
        message.clear();
        latestFramePath.clear();
        queue.clear();
    }
};

PngSequenceWriter::PngSequenceWriter(uint32_t capacity) : mImpl(std::make_unique<Impl>(capacity)) {}

PngSequenceWriter::~PngSequenceWriter() = default;

bool PngSequenceWriter::open(CompiledExportPlan const& plan) {
    mImpl->requestCancel();
    mImpl->wait();
    mImpl->resetForOpen();

    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->directory = plan.outputPath;
    }

    std::error_code ec;
    bool const      outputExists = std::filesystem::exists(mImpl->directory, ec);
    if (ec) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(ExportError::InvalidOutputPath, "Unable to inspect the export output path");
        return false;
    }
    if (outputExists) {
        if (!std::filesystem::is_directory(mImpl->directory, ec) || ec) {
            std::scoped_lock lock(mImpl->mutex);
            mImpl->setFailureLocked(ExportError::InvalidOutputPath, "The export output path is not a directory");
            return false;
        }
        auto const entry = std::filesystem::directory_iterator(mImpl->directory, ec);
        if (ec || entry != std::filesystem::directory_iterator{}) {
            std::scoped_lock lock(mImpl->mutex);
            mImpl->setFailureLocked(ExportError::OutputExists, "The export output directory is not empty");
            return false;
        }
    } else if (!std::filesystem::create_directories(mImpl->directory, ec) || ec) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(ExportError::DirectoryCreateFailed, "Unable to create the export output directory");
        return false;
    } else {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->ownsDirectory = true;
    }

    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->state = FrameWriterState::Running;
    }
    mImpl->worker = std::thread([impl = mImpl.get()] { impl->workerLoop(); });
    return true;
}

FrameWriterSubmitResult PngSequenceWriter::trySubmit(functions::render::CapturedFrame& frame) {
    std::scoped_lock lock(mImpl->mutex);
    if (mImpl->error != ExportError::None) return FrameWriterSubmitResult::Failed;
    if (mImpl->state != FrameWriterState::Running) return FrameWriterSubmitResult::Closed;
    if (!detail::validateFrame(frame)) {
        mImpl->setFailureLocked(ExportError::InvalidFrame, "The captured frame has invalid dimensions or pixels");
        mImpl->changed.notify_all();
        return FrameWriterSubmitResult::Failed;
    }
    if (frame.ticket.frameIndex != mImpl->nextFrameIndex) {
        mImpl->setFailureLocked(ExportError::FrameOutOfOrder, "Captured frames were submitted out of order");
        mImpl->changed.notify_all();
        return FrameWriterSubmitResult::Failed;
    }
    if (mImpl->frameWidth == 0) {
        mImpl->frameWidth  = frame.width;
        mImpl->frameHeight = frame.height;
    } else if (frame.width != mImpl->frameWidth || frame.height != mImpl->frameHeight) {
        mImpl->setFailureLocked(ExportError::InvalidFrame, "Captured frame dimensions changed during export");
        mImpl->changed.notify_all();
        return FrameWriterSubmitResult::Failed;
    }
    if (mImpl->queue.size() >= mImpl->capacity) return FrameWriterSubmitResult::Backpressured;
    mImpl->queue.push_back({std::move(frame), mImpl->nextFrameIndex++});
    ++mImpl->submitted;
    mImpl->changed.notify_all();
    return FrameWriterSubmitResult::Accepted;
}

bool PngSequenceWriter::requestFinish() {
    std::scoped_lock lock(mImpl->mutex);
    if (mImpl->state == FrameWriterState::Finishing) return true;
    if (mImpl->state != FrameWriterState::Running) return false;
    mImpl->state = FrameWriterState::Finishing;
    mImpl->changed.notify_all();
    return true;
}

void PngSequenceWriter::requestCancel() { mImpl->requestCancel(); }

void PngSequenceWriter::wait() { mImpl->wait(); }

FrameWriterStatus PngSequenceWriter::status() const {
    std::scoped_lock lock(mImpl->mutex);
    return FrameWriterStatus{
        mImpl->state,
        mImpl->submitted,
        mImpl->written,
        mImpl->error,
        mImpl->message,
        mImpl->latestFramePath
    };
}

} // namespace playback::editor::exporting
