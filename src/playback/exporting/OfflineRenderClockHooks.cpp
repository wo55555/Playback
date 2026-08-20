#include "OfflineRenderClockHooks.h"

#include "ExportActivity.h"
#include "playback/Playback.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/external/bgfx/Context.h"
#include "mc/external/bgfx/Frame.h"
#include "mc/platform/threading/Mutex.h"
#include "mc/util/Timer.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace playback::exporting {

namespace {

struct ActiveClockSample {
    OfflineRenderClockToken                     token;
    OfflineRenderClockSample                    sample;
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    uint64_t                                    renderSerial{};
    void const*                                 expectedBgfxFrame{};
    void const*                                 submittedBgfxFrame{};
    uint32_t                                    expectedBgfxFrameNumber{};
    uint32_t                                    gameRenderOrdinal{};
    bool                                        claimed{};
    bool                                        renderReady{};
    bool                                        boundaryClaimed{};
    bool                                        cameraRequired{};
    bool                                        entityApplied{};
    bool                                        renderReturned{};
    bool                                        cameraApplicationFailureLogged{};
    bool                                        completed{};
};

struct AcquiredClockSample {
    OfflineRenderClockToken                     token;
    OfflineRenderClockSample                    sample;
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    uint64_t                                    renderSerial{};
};

struct ActiveRenderSample {
    visuals::ReplaySampleTime time;
    OfflineRenderClockToken   token;
    uint64_t                  renderSerial{};
    uint32_t                  gameRenderCalls{};
};

std::atomic_bool                               gHookInstalled{false};
std::atomic_bool                               gOfflineFlagMismatchLogged{false};
std::atomic_bool                               gInitialCameraSampleLogged{false};
std::atomic_bool                               gMissingCameraSampleLogged{false};
std::mutex                                     gClockMutex;
std::optional<ActiveClockSample>               gActiveSample;
uint64_t                                       gNextTokenId{1};
uint64_t                                       gNextRenderSerial{1};
thread_local std::optional<ActiveRenderSample> gRenderSample;

class ScopedRenderSample {
public:
    explicit ScopedRenderSample(
        visuals::ReplaySampleTime time,
        OfflineRenderClockToken   token        = {},
        uint64_t                  renderSerial = 0
    )
    : mPrevious(gRenderSample),
      mHadPrevious(gRenderSample.has_value()) {
        gRenderSample = ActiveRenderSample{time, token, renderSerial};
    }

    ~ScopedRenderSample() {
        if (mHadPrevious) gRenderSample = mPrevious;
        else gRenderSample.reset();
    }

    ScopedRenderSample(ScopedRenderSample const&)            = delete;
    ScopedRenderSample& operator=(ScopedRenderSample const&) = delete;

private:
    std::optional<ActiveRenderSample> mPrevious;
    bool                              mHadPrevious{};
};

class ScopedTimerOverride {
public:
    ScopedTimerOverride(Timer const& timer, OfflineRenderClockSample const& sample)
    : mTimer(const_cast<Timer&>(timer)),
      mTicksPerSecond(mTimer.mTicksPerSecond),
      mTicks(mTimer.mTicks),
      mAlpha(mTimer.mAlpha),
      mTimeScale(mTimer.mTimeScale),
      mPassedTime(mTimer.mPassedTime),
      mFrameStepAlignmentRemainder(mTimer.mFrameStepAlignmentRemainder),
      mLastTimeSeconds(mTimer.mLastTimeSeconds),
      mLastTimestep(mTimer.mLastTimestep),
      mOverflowTime(mTimer.mOverflowTime),
      mLastMs(mTimer.mLastMs),
      mLastMsSysTime(mTimer.mLastMsSysTime),
      mAdjustTime(mTimer.mAdjustTime),
      mSteppingTick(mTimer.mSteppingTick) {
        constexpr float ticksPerSecond = 20.0f;
        auto const      absoluteTick   = sample.replayTime.value();
        auto const      partialTick    = sample.replayTime.partialTick();
        auto const      absoluteMilliseconds =
            static_cast<int64>(std::llround(absoluteTick * 1000.0L / static_cast<long double>(ticksPerSecond)));

        mTimer.mTicksPerSecond              = ticksPerSecond;
        mTimer.mTicks                       = sample.wholeTicks;
        mTimer.mAlpha                       = partialTick;
        mTimer.mTimeScale                   = 1.0f;
        mTimer.mPassedTime                  = sample.deltaTicks;
        mTimer.mFrameStepAlignmentRemainder = 0.0f;
        mTimer.mLastTimeSeconds             = static_cast<float>(absoluteTick / ticksPerSecond);
        mTimer.mLastTimestep                = sample.deltaTicks / ticksPerSecond;
        mTimer.mOverflowTime                = 0.0f;
        mTimer.mLastMs                      = absoluteMilliseconds;
        mTimer.mLastMsSysTime               = absoluteMilliseconds;
        mTimer.mAdjustTime                  = 0.0f;
        mTimer.mSteppingTick                = partialTick;
    }

    ~ScopedTimerOverride() {
        mTimer.mTicksPerSecond              = mTicksPerSecond;
        mTimer.mTicks                       = mTicks;
        mTimer.mAlpha                       = mAlpha;
        mTimer.mTimeScale                   = mTimeScale;
        mTimer.mPassedTime                  = mPassedTime;
        mTimer.mFrameStepAlignmentRemainder = mFrameStepAlignmentRemainder;
        mTimer.mLastTimeSeconds             = mLastTimeSeconds;
        mTimer.mLastTimestep                = mLastTimestep;
        mTimer.mOverflowTime                = mOverflowTime;
        mTimer.mLastMs                      = mLastMs;
        mTimer.mLastMsSysTime               = mLastMsSysTime;
        mTimer.mAdjustTime                  = mAdjustTime;
        mTimer.mSteppingTick                = mSteppingTick;
    }

    ScopedTimerOverride(ScopedTimerOverride const&)            = delete;
    ScopedTimerOverride& operator=(ScopedTimerOverride const&) = delete;

private:
    Timer& mTimer;
    float  mTicksPerSecond;
    int    mTicks;
    float  mAlpha;
    float  mTimeScale;
    float  mPassedTime;
    float  mFrameStepAlignmentRemainder;
    float  mLastTimeSeconds;
    float  mLastTimestep;
    float  mOverflowTime;
    int64  mLastMs;
    int64  mLastMsSysTime;
    float  mAdjustTime;
    float  mSteppingTick;
};

std::optional<AcquiredClockSample> acquireClockSampleForRender() {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->claimed) return std::nullopt;

    gActiveSample->claimed                        = true;
    gActiveSample->renderSerial                   = gNextRenderSerial++;
    gActiveSample->expectedBgfxFrame              = nullptr;
    gActiveSample->submittedBgfxFrame             = nullptr;
    gActiveSample->expectedBgfxFrameNumber        = 0;
    gActiveSample->gameRenderOrdinal              = 0;
    gActiveSample->renderReady                    = false;
    gActiveSample->boundaryClaimed                = false;
    gActiveSample->entityApplied                  = false;
    gActiveSample->renderReturned                 = false;
    gActiveSample->cameraApplicationFailureLogged = false;
    gActiveSample->completed                      = false;
    if (gNextRenderSerial == 0) ++gNextRenderSerial;
    return AcquiredClockSample{
        gActiveSample->token,
        gActiveSample->sample,
        gActiveSample->cameraContext,
        gActiveSample->renderSerial,
    };
}

void completeClockSample(OfflineRenderClockToken token, uint64_t renderSerial, bool entityApplied) {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id || gActiveSample->renderSerial != renderSerial) return;
    gActiveSample->entityApplied  = entityApplied;
    gActiveSample->renderReturned = true;
}

void markClockSampleRenderReady(OfflineRenderClockToken token, uint64_t renderSerial, bool ready) {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id || gActiveSample->renderSerial != renderSerial) return;
    gActiveSample->renderReady = ready;
}

void recordGameRenderCall(OfflineRenderClockToken token, uint64_t renderSerial, uint32_t ordinal) {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id || gActiveSample->renderSerial != renderSerial) {
        return;
    }
    gActiveSample->gameRenderOrdinal = ordinal;
}

bool recordClockSampleBgfxFrame(
    OfflineRenderClockToken token,
    uint64_t                renderSerial,
    void const*             frame,
    uint32_t                frameNumber,
    uint32_t                gameRenderOrdinal,
    bool                    requireScopedSample
) {
    if (!frame) return false;

    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || !gActiveSample->claimed || !gActiveSample->renderReady || gActiveSample->expectedBgfxFrame
        || gActiveSample->completed) {
        return false;
    }
    if (requireScopedSample && (gActiveSample->token.id != token.id || gActiveSample->renderSerial != renderSerial)) {
        return false;
    }

    gActiveSample->expectedBgfxFrame       = frame;
    gActiveSample->expectedBgfxFrameNumber = frameNumber;
    if (gameRenderOrdinal != 0) gActiveSample->gameRenderOrdinal = gameRenderOrdinal;
    return true;
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderClockUpdateGraphicsHook,
    ll::memory::HookPriority::Lowest,
    MinecraftGame,
    &MinecraftGame::updateGraphics,
    void,
    Bedrock::NotNullNonOwnerPtr<IClientInstance> const& client,
    Timer const&                                        timer
) {
    auto const sample = acquireClockSampleForRender();
    if (sample) {
        bool poseApplied = false;
        {
            ScopedTimerOverride timerOverride(timer, sample->sample);
            ScopedRenderSample  renderSample(sample->sample.replayTime, sample->token, sample->renderSerial);
            keyframe::ScopedCameraTimelineRenderContext cameraContext(sample->cameraContext);
            auto pose   = replay::ReplaySession::getInstance().createReplayEntityRenderScope(sample->sample.replayTime);
            poseApplied = pose != nullptr;

            markClockSampleRenderReady(sample->token, sample->renderSerial, true);
            origin(client, timer);
        }
        completeClockSample(sample->token, sample->renderSerial, poseApplied);
        return;
    }

    if (isOfflineRenderActivityActive()) {
        if (isExportActivityActive()) return;
        setOfflineRenderActivityActive(false);
        if (!gOfflineFlagMismatchLogged.exchange(true, std::memory_order_acq_rel)) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Offline render flag was active without an export; cleared stale state and resumed native graphics"
            );
        }
    }

    origin(client, timer);
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderGameFrameHook,
    ll::memory::HookPriority::Highest,
    GameRenderer,
    &GameRenderer::renderCurrentFrame,
    void,
    float partialTick
) {
    auto* const sample = gRenderSample ? &*gRenderSample : nullptr;
    if (sample && sample->token) {
        ++sample->gameRenderCalls;
        recordGameRenderCall(sample->token, sample->renderSerial, sample->gameRenderCalls);
    }
    origin(partialTick);
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderBgfxSwapHook,
    ll::memory::HookPriority::Highest,
    bgfx::Context,
    &bgfx::Context::swap,
    void
) {
    auto* const sample = gRenderSample ? &*gRenderSample : nullptr;
    auto* const frame  = this->m_submit;
    if (sample && sample->token && sample->renderSerial != 0) {
        recordClockSampleBgfxFrame(
            sample->token,
            sample->renderSerial,
            frame,
            frame ? static_cast<uint32_t>(frame->m_frameNum) : 0,
            sample->gameRenderCalls,
            true
        );
    } else if (isOfflineRenderActivityActive()) {
        recordClockSampleBgfxFrame({}, 0, frame, frame ? static_cast<uint32_t>(frame->m_frameNum) : 0, 0, false);
    }
    origin();
}

} // namespace

bool hookOfflineRenderClock(bool enable) {
    struct HookState {
        bool clock{};
        bool gameFrame{};
        bool bgfxSwap{};
    };
    static HookState state;

    auto removeAll = [&] {
        gHookInstalled.store(false, std::memory_order_release);
        resetOfflineRenderClock();
        if (state.bgfxSwap && OfflineRenderBgfxSwapHook::unhook()) state.bgfxSwap = false;
        if (state.gameFrame && OfflineRenderGameFrameHook::unhook()) state.gameFrame = false;
        if (state.clock && OfflineRenderClockUpdateGraphicsHook::unhook()) state.clock = false;
        return !state.clock && !state.gameFrame && !state.bgfxSwap;
    };

    if (enable) {
        if (!state.clock) state.clock = OfflineRenderClockUpdateGraphicsHook::hook() == 0;
        if (!state.gameFrame) state.gameFrame = OfflineRenderGameFrameHook::hook() == 0;
        if (!state.bgfxSwap) state.bgfxSwap = OfflineRenderBgfxSwapHook::hook() == 0;
        bool const ready = state.clock && state.gameFrame && state.bgfxSwap;
        gHookInstalled.store(ready, std::memory_order_release);
        if (ready) {
            gInitialCameraSampleLogged.store(false, std::memory_order_release);
            gMissingCameraSampleLogged.store(false, std::memory_order_release);
            return true;
        }

        bool const clockInstalled     = state.clock;
        bool const gameFrameInstalled = state.gameFrame;
        bool const bgfxSwapInstalled  = state.bgfxSwap;
        bool const rolledBack         = removeAll();
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install export-scoped render hooks (updateGraphics={}, gameFrame={}, bgfxSwap={}, rollback={})",
            clockInstalled,
            gameFrameInstalled,
            bgfxSwapInstalled,
            rolledBack
        );
        return false;
    }

    return removeAll();
}

bool isOfflineRenderClockInstalled() { return gHookInstalled.load(std::memory_order_acquire); }

OfflineRenderClockPublishResult
publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token) {
    token = {};
    if (!sample.replayTime.isValid() || !std::isfinite(sample.deltaTicks) || sample.deltaTicks < 0.0f
        || sample.wholeTicks < 0) {
        return OfflineRenderClockPublishResult::InvalidSample;
    }
    if (!gHookInstalled.load(std::memory_order_acquire)) return OfflineRenderClockPublishResult::Unavailable;

    auto const cameraSample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Export, sample.replayTime);
    if (cameraSample && !editor::graphics::isCameraRenderInstalled())
        return OfflineRenderClockPublishResult::Unavailable;
    auto const cameraAppliedFlag =
        cameraSample ? std::make_shared<std::atomic_bool>(false) : keyframe::CameraTimelineAppliedFlag{};
    {
        std::scoped_lock lock(gClockMutex);
        if (!gHookInstalled.load(std::memory_order_relaxed)) return OfflineRenderClockPublishResult::Unavailable;
        if (gActiveSample) return OfflineRenderClockPublishResult::Busy;

        token.id = gNextTokenId++;
        if (gNextTokenId == 0) ++gNextTokenId;

        auto const cameraContext = keyframe::publishCameraTimelineRenderContext(keyframe::CameraTimelineRenderContext{
            sample.replayTime,
            keyframe::CameraTimelineSource::Export,
            token.id,
            cameraSample,
            cameraAppliedFlag,
            sample.frameIndex,
        });
        ActiveClockSample active{};
        active.token          = token;
        active.sample         = sample;
        active.cameraContext  = cameraContext;
        active.cameraRequired = cameraSample.has_value();
        gActiveSample         = std::move(active);
    }
    bool const logSample  = cameraSample && !gInitialCameraSampleLogged.exchange(true, std::memory_order_acq_rel);
    bool const logMissing = !cameraSample && !gMissingCameraSampleLogged.exchange(true, std::memory_order_acq_rel);
    if (logSample || logMissing) {
        auto& logger = Playback::getInstance().getSelf().getLogger();
        if (cameraSample) {
            auto const& state = cameraSample->state;
            logger.debug(
                "Initial export camera sample (frame={}, token={}, tick={}/{}, cameraId={}, position=({}, {}, {}), "
                "yaw={}, pitch={}, roll={}, fov={})",
                sample.frameIndex,
                token.id,
                sample.replayTime.numerator,
                sample.replayTime.denominator,
                cameraSample->cameraId,
                state.x,
                state.y,
                state.z,
                state.yaw,
                state.pitch,
                state.roll,
                state.fov
            );
        } else {
            logger.warn(
                "Export camera sample missing (frame={}, token={}, tick={}/{})",
                sample.frameIndex,
                token.id,
                sample.replayTime.numerator,
                sample.replayTime.denominator
            );
        }
    }
    return OfflineRenderClockPublishResult::Published;
}

bool wasOfflineRenderClockSampleApplied(OfflineRenderClockToken token) {
    if (!token) return false;
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id || !gActiveSample->renderReady
        || !gActiveSample->renderReturned) {
        return false;
    }
    if (!gActiveSample->cameraRequired) return true;
    auto const& cameraContext = gActiveSample->cameraContext;
    bool const  applied =
        cameraContext && cameraContext->appliedFlag && cameraContext->appliedFlag->load(std::memory_order_acquire);
    if (!applied && !gActiveSample->cameraApplicationFailureLogged) {
        gActiveSample->cameraApplicationFailureLogged = true;
        auto const publishedContext                   = keyframe::currentCameraTimelineRenderContext();
        Playback::getInstance().getSelf().getLogger().error(
            "Export camera sample missed the final ViewRenderObject boundary (frame={}, token={}, renderSerial={}, "
            "tick={}/{}, contextPresent={}, contextMatches={}, publishedToken={})",
            gActiveSample->sample.frameIndex,
            gActiveSample->token.id,
            gActiveSample->renderSerial,
            gActiveSample->sample.replayTime.numerator,
            gActiveSample->sample.replayTime.denominator,
            publishedContext != nullptr,
            publishedContext == cameraContext,
            publishedContext ? publishedContext->renderToken : 0
        );
    }
    return applied;
}

bool wasOfflineRenderClockSampleCompleted(OfflineRenderClockToken token) {
    if (!token) return false;
    std::scoped_lock lock(gClockMutex);
    return gActiveSample && gActiveSample->token.id == token.id && gActiveSample->completed;
}

std::optional<OfflineRenderBoundaryTicket> claimOfflineRenderBoundary(void const* frame, uint32_t frameNumber) {
    if (!frame) return std::nullopt;
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || !gActiveSample->claimed || !gActiveSample->renderReady || gActiveSample->boundaryClaimed
        || gActiveSample->completed || gActiveSample->submittedBgfxFrame || gActiveSample->expectedBgfxFrame != frame
        || gActiveSample->expectedBgfxFrameNumber != frameNumber) {
        return std::nullopt;
    }

    auto& sample              = *gActiveSample;
    sample.boundaryClaimed    = true;
    sample.submittedBgfxFrame = frame;
    return OfflineRenderBoundaryTicket{
        sample.token.id,
        sample.sample.frameIndex,
        sample.renderSerial,
        frame,
        frameNumber,
        sample.gameRenderOrdinal,
    };
}

std::optional<OfflineRenderBoundaryTicket> claimOfflineRenderPresentFallback() {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || !gActiveSample->claimed || !gActiveSample->renderReady || gActiveSample->boundaryClaimed
        || gActiveSample->completed) {
        return std::nullopt;
    }

    auto& sample           = *gActiveSample;
    sample.boundaryClaimed = true;
    return OfflineRenderBoundaryTicket{
        sample.token.id,
        sample.sample.frameIndex,
        sample.renderSerial,
        nullptr,
        0,
        sample.gameRenderOrdinal,
    };
}

void markOfflineRenderBoundaryCompleted(OfflineRenderBoundaryTicket const& ticket) {
    if (ticket.clockToken == 0 || ticket.renderSerial == 0) return;
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != ticket.clockToken
        || gActiveSample->renderSerial != ticket.renderSerial || !gActiveSample->boundaryClaimed
        || (ticket.bgfxFrame && gActiveSample->submittedBgfxFrame != ticket.bgfxFrame)
        || (ticket.bgfxFrame && gActiveSample->expectedBgfxFrameNumber != ticket.bgfxFrameNumber)) {
        return;
    }
    if (gActiveSample->sample.frameIndex < 2 || gActiveSample->sample.frameIndex % 60 == 0
        || gActiveSample->gameRenderOrdinal != 1 || !gActiveSample->entityApplied) {
        Playback::getInstance().getSelf().getLogger().debug(
            "Export render boundary completed (frame={}, token={}, renderSerial={}, gameRenderCalls={}, "
            "entityApplied={}, bgfxFrameNumber={})",
            gActiveSample->sample.frameIndex,
            gActiveSample->token.id,
            gActiveSample->renderSerial,
            gActiveSample->gameRenderOrdinal,
            gActiveSample->entityApplied,
            ticket.bgfxFrameNumber
        );
    }
    gActiveSample->completed = true;
}

void clearOfflineRenderClockSample(OfflineRenderClockToken token) {
    if (!token) return;
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    {
        std::scoped_lock lock(gClockMutex);
        if (!gActiveSample || gActiveSample->token.id != token.id) return;
        cameraContext = std::move(gActiveSample->cameraContext);
        gActiveSample.reset();
    }
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Export, cameraContext);
}

void resetOfflineRenderClock() {
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    {
        std::scoped_lock lock(gClockMutex);
        if (gActiveSample) cameraContext = std::move(gActiveSample->cameraContext);
        gActiveSample.reset();
    }
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Export, cameraContext);
    gRenderSample.reset();
}

} // namespace playback::exporting
