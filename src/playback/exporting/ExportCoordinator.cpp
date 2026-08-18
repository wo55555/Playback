#include "ExportCoordinator.h"

#include "ExportPlanCompiler.h"
#include "FfmpegVideoWriter.h"
#include "PngSequenceWriter.h"

#include <mutex>
#include <utility>

namespace playback::exporting {

namespace {

[[nodiscard]] std::unique_ptr<IFrameWriter> makeDefaultWriter(ExportFormat format) {
    if (format == ExportFormat::Mp4Video) return std::make_unique<FfmpegVideoWriter>();
    return std::make_unique<PngSequenceWriter>();
}

} // namespace

struct ExportCoordinator::Impl {
    explicit Impl(WriterFactory writerFactory) : factory(std::move(writerFactory)) {}

    WriterFactory                     factory;
    std::unique_ptr<IFrameWriter>     writer;
    std::optional<CompiledExportPlan> compiledPlan;
    std::mutex                        operationMutex;
    mutable std::mutex                mutex;
    ExportStatus                      currentStatus;
    ExportError                       pendingError{ExportError::None};
    std::string                       pendingMessage;

    void setFailureLocked(ExportError error, std::string message) {
        currentStatus.state   = ExportState::Faulted;
        currentStatus.error   = error;
        currentStatus.message = std::move(message);
        pendingError          = ExportError::None;
        pendingMessage.clear();
    }

    void requestFailureLocked(ExportError error, std::string message) {
        if (pendingError == ExportError::None) {
            pendingError   = error;
            pendingMessage = std::move(message);
        }
        currentStatus.state = writer ? ExportState::Cancelling : ExportState::Faulted;
        currentStatus.error = ExportError::None;
    }

    void refreshWriterStatusLocked() {
        if (!writer) return;
        auto const writerStatus       = writer->status();
        currentStatus.format          = compiledPlan ? compiledPlan->settings.format : currentStatus.format;
        currentStatus.submittedFrames = writerStatus.submittedFrames;
        currentStatus.writtenFrames   = writerStatus.writtenFrames;
        currentStatus.latestFramePath = writerStatus.latestFramePath;

        if ((currentStatus.state == ExportState::Running || currentStatus.state == ExportState::Finalizing)
            && writerStatus.error != ExportError::None) {
            requestFailureLocked(writerStatus.error, writerStatus.message);
            writer->requestCancel();
            return;
        }
        if (currentStatus.state == ExportState::Finalizing && writerStatus.state == FrameWriterState::Completed) {
            currentStatus.state = ExportState::Completed;
            currentStatus.error = ExportError::None;
            return;
        }
        if (currentStatus.state != ExportState::Cancelling) return;

        if (writerStatus.state == FrameWriterState::Completed) {
            currentStatus.state = ExportState::Completed;
            currentStatus.error = ExportError::None;
        } else if (writerStatus.state == FrameWriterState::Cancelled) {
            if (pendingError != ExportError::None) {
                setFailureLocked(pendingError, pendingMessage);
            } else {
                currentStatus.state   = ExportState::Cancelled;
                currentStatus.error   = ExportError::Cancelled;
                currentStatus.message = "Export cancelled";
            }
        } else if (writerStatus.state == FrameWriterState::Faulted) {
            setFailureLocked(
                pendingError != ExportError::None ? pendingError : writerStatus.error,
                !pendingMessage.empty()
                    ? pendingMessage
                    : (writerStatus.message.empty() ? "The export writer failed" : writerStatus.message)
            );
        }
    }
};

ExportCoordinator::ExportCoordinator() : mImpl(std::make_unique<Impl>(makeDefaultWriter)) {}

ExportCoordinator::ExportCoordinator(WriterFactory factory)
: mImpl(std::make_unique<Impl>(factory ? std::move(factory) : WriterFactory(makeDefaultWriter))) {}

ExportCoordinator::~ExportCoordinator() { reset(); }

bool ExportCoordinator::start(ExportSettings settings, state::editing::model::EditorStateExt const& project) {
    std::scoped_lock              operationLock(mImpl->operationMutex);
    std::unique_ptr<IFrameWriter> previousWriter;
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->refreshWriterStatusLocked();
        if (isExportActive(mImpl->currentStatus.state)) return false;
        previousWriter = std::move(mImpl->writer);
        mImpl->compiledPlan.reset();
        mImpl->pendingError = ExportError::None;
        mImpl->pendingMessage.clear();
        mImpl->currentStatus        = {};
        mImpl->currentStatus.state  = ExportState::Preparing;
        mImpl->currentStatus.format = settings.format;
    }
    if (previousWriter) {
        previousWriter->requestCancel();
        previousWriter->wait();
    }

    auto compiled = ExportPlanCompiler::compile(settings, project);
    if (!compiled) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(compiled.error, std::move(compiled.message));
        return false;
    }

    auto writer = mImpl->factory ? mImpl->factory(compiled.plan->settings.format) : nullptr;
    if (!writer) {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(ExportError::WriterUnavailable, "No export frame writer is available");
        return false;
    }
    if (!writer->open(*compiled.plan)) {
        auto const       writerStatus = writer->status();
        std::scoped_lock lock(mImpl->mutex);
        mImpl->setFailureLocked(
            writerStatus.error == ExportError::None ? ExportError::WriterUnavailable : writerStatus.error,
            writerStatus.message.empty() ? "Unable to open the export frame writer" : writerStatus.message
        );
        return false;
    }

    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->compiledPlan              = std::move(compiled.plan);
        mImpl->writer                    = std::move(writer);
        mImpl->currentStatus.state       = ExportState::Running;
        mImpl->currentStatus.format      = mImpl->compiledPlan->settings.format;
        mImpl->currentStatus.outputPath  = mImpl->compiledPlan->outputPath;
        mImpl->currentStatus.totalFrames = mImpl->compiledPlan->frameCount;
    }
    return true;
}

FrameWriterSubmitResult ExportCoordinator::trySubmit(visuals::CapturedFrame& frame) {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    bool             invalidTicket{};
    {
        std::scoped_lock lock(mImpl->mutex);
        if (mImpl->currentStatus.state != ExportState::Running || !mImpl->writer || !mImpl->compiledPlan) {
            return mImpl->currentStatus.state == ExportState::Faulted ? FrameWriterSubmitResult::Failed
                                                                      : FrameWriterSubmitResult::Closed;
        }
        auto const expected = mImpl->compiledPlan->frame(frame.ticket.frameIndex);
        if (!expected || frame.ticket.ptsNumerator != expected->ticket.ptsNumerator
            || frame.ticket.ptsDenominator != expected->ticket.ptsDenominator) {
            mImpl->requestFailureLocked(
                ExportError::InvalidFrame,
                "The captured frame ticket does not match the export plan"
            );
            invalidTicket = true;
        }
        writer = mImpl->writer.get();
    }
    if (invalidTicket) {
        writer->requestCancel();
        return FrameWriterSubmitResult::Failed;
    }

    auto const result       = writer->trySubmit(frame);
    auto const writerStatus = writer->status();
    bool       writerFailed = result == FrameWriterSubmitResult::Failed || writerStatus.error != ExportError::None;
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->currentStatus.submittedFrames = writerStatus.submittedFrames;
        mImpl->currentStatus.writtenFrames   = writerStatus.writtenFrames;
        mImpl->currentStatus.latestFramePath = writerStatus.latestFramePath;
        if (writerFailed) {
            mImpl->requestFailureLocked(
                writerStatus.error == ExportError::None ? ExportError::WriteFailed : writerStatus.error,
                writerStatus.message.empty() ? "The export frame writer failed" : writerStatus.message
            );
        }
    }
    if (writerFailed) writer->requestCancel();
    return result;
}

bool ExportCoordinator::finish() {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    bool             frameCountMismatch{};
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->refreshWriterStatusLocked();
        if (mImpl->currentStatus.state != ExportState::Running || !mImpl->writer || !mImpl->compiledPlan) return false;
        if (mImpl->currentStatus.submittedFrames != mImpl->compiledPlan->frameCount) {
            mImpl->requestFailureLocked(
                ExportError::FrameCountMismatch,
                "The export ended before all frames were submitted"
            );
            frameCountMismatch = true;
        } else {
            mImpl->currentStatus.state = ExportState::Finalizing;
        }
        writer = mImpl->writer.get();
    }
    if (frameCountMismatch) {
        writer->requestCancel();
        return false;
    }
    return writer->requestFinish();
}

void ExportCoordinator::fail(ExportError error, std::string message) {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    {
        std::scoped_lock lock(mImpl->mutex);
        if (!isExportActive(mImpl->currentStatus.state)) {
            mImpl->setFailureLocked(error, std::move(message));
            return;
        }
        mImpl->requestFailureLocked(error, std::move(message));
        writer = mImpl->writer.get();
    }
    if (writer) writer->requestCancel();
}

void ExportCoordinator::cancel() {
    std::scoped_lock operationLock(mImpl->operationMutex);
    IFrameWriter*    writer = nullptr;
    {
        std::scoped_lock lock(mImpl->mutex);
        mImpl->refreshWriterStatusLocked();
        if (!isExportActive(mImpl->currentStatus.state) || !mImpl->writer) return;
        mImpl->pendingError = ExportError::None;
        mImpl->pendingMessage.clear();
        mImpl->currentStatus.state   = ExportState::Cancelling;
        mImpl->currentStatus.error   = ExportError::None;
        mImpl->currentStatus.message = "Cancelling export";
        writer                       = mImpl->writer.get();
    }
    writer->requestCancel();
}

void ExportCoordinator::reset() {
    std::scoped_lock              operationLock(mImpl->operationMutex);
    std::unique_ptr<IFrameWriter> writer;
    {
        std::scoped_lock lock(mImpl->mutex);
        writer = std::move(mImpl->writer);
        mImpl->compiledPlan.reset();
        mImpl->pendingError = ExportError::None;
        mImpl->pendingMessage.clear();
        mImpl->currentStatus = {};
    }
    if (writer) {
        writer->requestCancel();
        writer->wait();
    }
}

ExportStatus ExportCoordinator::status() const {
    std::scoped_lock lock(mImpl->mutex);
    mImpl->refreshWriterStatusLocked();
    return mImpl->currentStatus;
}

std::optional<CompiledExportPlan> ExportCoordinator::plan() const {
    std::scoped_lock lock(mImpl->mutex);
    return mImpl->compiledPlan;
}

bool ExportCoordinator::isFormatAvailable(ExportFormat format) {
    return format == ExportFormat::PngSequence || FfmpegVideoWriter::isAvailable();
}

} // namespace playback::exporting
