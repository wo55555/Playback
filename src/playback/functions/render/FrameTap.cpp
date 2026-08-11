#include "FrameTap.h"

#include <utility>

namespace playback::functions::render {

namespace {

constexpr uint32_t MaxFrameTapCapacity = 8;

bool isTerminal(FrameTapState state) {
    return state == FrameTapState::Completed || state == FrameTapState::Cancelled || state == FrameTapState::Faulted;
}

} // namespace

FrameTapOpenResult FrameTap::open(FrameTapConfig config, FrameTapSession& session) {
    std::scoped_lock lock(mMutex);
    session = {};
    if (config.capacity == 0 || config.capacity > MaxFrameTapCapacity) return FrameTapOpenResult::InvalidConfig;
    if (mActive) return FrameTapOpenResult::Busy;

    if (mNextSessionId == 0) ++mNextSessionId;
    ActiveSession active;
    active.handle = FrameTapSession{mNextSessionId++};
    active.config = config;
    session       = active.handle;
    mActive       = std::move(active);
    return FrameTapOpenResult::Opened;
}

void FrameTap::close(FrameTapSession session) {
    {
        std::scoped_lock lock(mMutex);
        if (!matches(session)) return;
        mActive.reset();
    }
    mChanged.notify_all();
}

void FrameTap::cancel(FrameTapSession session) {
    {
        std::scoped_lock lock(mMutex);
        if (!matches(session)) return;
        mActive->state = FrameTapState::Cancelled;
        mActive->error = FrameTapError::Cancelled;
        mActive->message.assign("Frame capture was cancelled");
        mActive->armedTicket.reset();
        mActive->startedCaptures.clear();
        mActive->readyFrames.clear();
        mActive->inFlightFrames = 0;
    }
    mChanged.notify_all();
}

FrameTapArmResult FrameTap::tryArm(FrameTapSession session, FrameTicket ticket) {
    std::scoped_lock lock(mMutex);
    if (!matches(session) || mActive->state != FrameTapState::Active) return FrameTapArmResult::Inactive;
    if (ticket.ptsDenominator <= 0) return FrameTapArmResult::InvalidTicket;
    if (mActive->armedTicket) return FrameTapArmResult::Busy;
    if (mActive->config.oneShot && mActive->submittedFrames != 0) return FrameTapArmResult::Inactive;

    auto const outstanding = mActive->readyFrames.size() + mActive->inFlightFrames;
    if (outstanding >= mActive->config.capacity) return FrameTapArmResult::Backpressured;
    mActive->armedTicket = ticket;
    return FrameTapArmResult::Armed;
}

std::optional<FrameTapBackendCapture> FrameTap::tryPopStarted(FrameTapSession session) {
    std::scoped_lock lock(mMutex);
    if (!matches(session) || mActive->startedCaptures.empty()) return std::nullopt;
    auto capture = std::move(mActive->startedCaptures.front());
    mActive->startedCaptures.pop_front();
    return capture;
}

std::optional<CapturedFrame> FrameTap::tryPop(FrameTapSession session) {
    std::scoped_lock lock(mMutex);
    if (!matches(session)) return std::nullopt;
    return popReadyFrame();
}

std::optional<CapturedFrame> FrameTap::waitPop(FrameTapSession session, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mMutex);
    if (!matches(session)) return std::nullopt;
    mChanged.wait_for(lock, timeout, [this, session] {
        return !matches(session) || !mActive->readyFrames.empty() || isTerminal(mActive->state);
    });
    if (!matches(session)) return std::nullopt;
    return popReadyFrame();
}

FrameTapStatus FrameTap::status(FrameTapSession session) const {
    std::scoped_lock lock(mMutex);
    FrameTapStatus   result;
    if (!mActive || (session && !matches(session))) return result;

    result.state          = mActive->state;
    result.error          = mActive->error;
    result.message        = mActive->message;
    result.bufferedFrames = static_cast<uint32_t>(mActive->readyFrames.size());
    result.inFlightFrames = mActive->inFlightFrames;
    result.armed          = mActive->armedTicket.has_value();
    return result;
}

bool FrameTap::hasArmedCapture() const {
    std::scoped_lock lock(mMutex);
    return mActive && mActive->state == FrameTapState::Active && mActive->armedTicket.has_value();
}

bool FrameTap::requiresRenderPass() const {
    std::scoped_lock lock(mMutex);
    return mActive && mActive->state == FrameTapState::Active
        && (mActive->armedTicket.has_value() || mActive->inFlightFrames != 0);
}

uint32_t FrameTap::captureCapacity() const {
    std::scoped_lock lock(mMutex);
    return mActive ? mActive->config.capacity : 0;
}

std::optional<FrameTapBackendCapture> FrameTap::beginCapture() {
    FrameTapBackendCapture capture;
    {
        std::scoped_lock lock(mMutex);
        if (!mActive || mActive->state != FrameTapState::Active || !mActive->armedTicket) return std::nullopt;
        if (mActive->inFlightFrames + mActive->readyFrames.size() >= mActive->config.capacity) return std::nullopt;

        capture.session   = mActive->handle;
        capture.ticket    = *mActive->armedTicket;
        capture.captureId = mNextCaptureId++;
        mActive->armedTicket.reset();
        mActive->startedCaptures.emplace_back(capture);
        ++mActive->inFlightFrames;
        ++mActive->submittedFrames;
    }
    mChanged.notify_all();
    return capture;
}

void FrameTap::complete(FrameTapBackendCapture capture, CapturedFrame frame) {
    {
        std::scoped_lock lock(mMutex);
        if (!matches(capture.session) || mActive->state != FrameTapState::Active || mActive->inFlightFrames == 0)
            return;
        --mActive->inFlightFrames;
        frame.ticket     = capture.ticket;
        frame.submission = capture.submission;
        mActive->readyFrames.emplace_back(std::move(frame));
        if (mActive->config.oneShot) mActive->state = FrameTapState::Completed;
    }
    mChanged.notify_all();
}

void FrameTap::fail(FrameTapBackendCapture capture, FrameTapError error, std::string message) {
    {
        std::scoped_lock lock(mMutex);
        if (!matches(capture.session)) return;
        faultActive(error, std::move(message));
    }
    mChanged.notify_all();
}

void FrameTap::failActive(FrameTapError error, std::string message) {
    {
        std::scoped_lock lock(mMutex);
        if (!mActive || mActive->state != FrameTapState::Active
            || (!mActive->armedTicket && mActive->inFlightFrames == 0)) {
            return;
        }
        faultActive(error, std::move(message));
    }
    mChanged.notify_all();
}

bool FrameTap::matches(FrameTapSession session) const { return mActive && session && mActive->handle.id == session.id; }

std::optional<CapturedFrame> FrameTap::popReadyFrame() {
    if (!mActive || mActive->readyFrames.empty()) return std::nullopt;
    auto frame = std::move(mActive->readyFrames.front());
    mActive->readyFrames.pop_front();
    return frame;
}

void FrameTap::faultActive(FrameTapError error, std::string message) {
    mActive->state   = FrameTapState::Faulted;
    mActive->error   = error;
    mActive->message = std::move(message);
    mActive->armedTicket.reset();
    mActive->startedCaptures.clear();
    mActive->readyFrames.clear();
    mActive->inFlightFrames = 0;
}

} // namespace playback::functions::render
