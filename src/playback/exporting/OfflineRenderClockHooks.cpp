#include "OfflineRenderClockHooks.h"

#include "ExportActivity.h"
#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/renderer/game/GameRenderer.h"
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
    uint32_t                                    gameRenderOrdinal{};
    bool                                        claimed{};
    bool                                        renderReady{};
    bool                                        cameraRequired{};
    bool                                        entityApplied{};
    bool                                        renderReturned{};
    bool                                        cameraApplicationFailureLogged{};
};

struct AcquiredClockSample {
    OfflineRenderClockToken                     token;
    OfflineRenderClockSample                    sample;
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    uint64_t                                    renderSerial{};
};

struct ActiveRenderSample {
    OfflineRenderClockToken token;
    uint64_t                renderSerial{};
    uint32_t                gameRenderCalls{};
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
    explicit ScopedRenderSample(OfflineRenderClockToken token = {}, uint64_t renderSerial = 0)
    : mPrevious(gRenderSample) {
        gRenderSample = ActiveRenderSample{token, renderSerial};
    }

    ~ScopedRenderSample() { gRenderSample = mPrevious; }

    ScopedRenderSample(ScopedRenderSample const&)            = delete;
    ScopedRenderSample& operator=(ScopedRenderSample const&) = delete;

private:
    std::optional<ActiveRenderSample> mPrevious;
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
    if (!gActiveSample) return std::nullopt;
    // The frame is captured at Present, so a sample that already rendered must not be rendered again.
    if (gActiveSample->claimed && gActiveSample->renderReturned) return std::nullopt;

    gActiveSample->claimed                        = true;
    gActiveSample->renderSerial                   = gNextRenderSerial++;
    gActiveSample->gameRenderOrdinal              = 0;
    gActiveSample->renderReady                    = false;
    gActiveSample->entityApplied                  = false;
    gActiveSample->renderReturned                 = false;
    gActiveSample->cameraApplicationFailureLogged = false;
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

void recordGameRenderStart(OfflineRenderClockToken token, uint64_t renderSerial, uint32_t ordinal) {
    std::scoped_lock lock(gClockMutex);
    if (!gActiveSample || gActiveSample->token.id != token.id || gActiveSample->renderSerial != renderSerial) {
        return;
    }
    gActiveSample->gameRenderOrdinal = ordinal;
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
    // Export steps resolve on this path, so advance before rendering instead of waiting for the 20Hz client tick.
    if (isOfflineRenderActivityActive()) editor::tickReplayExportDuringGraphics();

    auto const sample = acquireClockSampleForRender();
    if (sample) {
        bool poseApplied = false;
        {
            ScopedTimerOverride                         timerOverride(timer, sample->sample);
            ScopedRenderSample                          renderSample(sample->token, sample->renderSerial);
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
    if (!sample || !sample->token) {
        origin(partialTick);
        return;
    }
    ++sample->gameRenderCalls;
    recordGameRenderStart(sample->token, sample->renderSerial, sample->gameRenderCalls);
    origin(partialTick);
}

} // namespace

bool hookOfflineRenderClock(bool enable) {
    struct HookState {
        bool clock{};
        bool gameFrame{};
    };
    static HookState state;

    auto removeAll = [&] {
        gHookInstalled.store(false, std::memory_order_release);
        resetOfflineRenderClock();
        if (state.gameFrame && OfflineRenderGameFrameHook::unhook()) state.gameFrame = false;
        if (state.clock && OfflineRenderClockUpdateGraphicsHook::unhook()) state.clock = false;
        return !state.clock && !state.gameFrame;
    };

    if (enable) {
        if (!state.clock) state.clock = OfflineRenderClockUpdateGraphicsHook::hook() == 0;
        if (!state.gameFrame) state.gameFrame = OfflineRenderGameFrameHook::hook() == 0;
        bool const ready = state.clock && state.gameFrame;
        gHookInstalled.store(ready, std::memory_order_release);
        if (ready) {
            gInitialCameraSampleLogged.store(false, std::memory_order_release);
            gMissingCameraSampleLogged.store(false, std::memory_order_release);
            return true;
        }

        bool const clockInstalled     = state.clock;
        bool const gameFrameInstalled = state.gameFrame;
        bool const rolledBack         = removeAll();
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install export-scoped render hooks (updateGraphics={}, gameFrame={}, rollback={})",
            clockInstalled,
            gameFrameInstalled,
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

    auto cameraSample = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Export, sample.replayTime);
    if (!cameraSample) {
        if (auto const viewpoint = replay::ReplaySession::getInstance().exportCameraViewpoint()) {
            cameraSample = keyframe::CameraTimelineSample{
                keyframe::CameraRenderState{
                                            viewpoint->x,
                                            viewpoint->y,
                                            viewpoint->z,
                                            viewpoint->yaw,
                                            viewpoint->pitch,
                                            viewpoint->roll,
                                            viewpoint->fov,
                                            },
                "__playback_export_observer__",
            };
        }
    }
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
        Playback::getInstance().getSelf().getLogger().error(
            "Export camera sample missed the final ViewRenderObject boundary (frame={})",
            gActiveSample->sample.frameIndex
        );
    }
    return applied;
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
