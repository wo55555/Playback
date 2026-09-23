#include "OfflineRenderClockHooks.h"

#include "ExportActivity.h"
#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/editor/graphics/CameraRenderHooks.h"
#include "playback/exporting/OfflineRenderTrace.h"
#include "playback/exporting/RenderDiagnostics.h"
#include "playback/keyframe/CameraTimelineRegistry.h"
#include "playback/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/multiplayer/MultiPlayerLevel.h"
#include "mc/client/renderer/game/GameRenderer.h"
#include "mc/common/Globals.h"
#include "mc/deps/minecraft_renderer/objects/FrameRenderObject.h"
#include "mc/deps/minecraft_renderer/objects/ViewRenderObject.h"
#include "mc/platform/threading/Mutex.h"
#include "mc/util/Timer.h"


#include <atomic>
#include <bit>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>


namespace playback::exporting {

namespace {

struct ActiveClockSample {
    OfflineRenderClockToken                     token;
    OfflineRenderClockSample                    sample;
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    uint64_t                                    captureGeneration{};
    uint64_t                                    renderSerial{};
    uint32_t                                    gameRenderOrdinal{};
    bool                                        captureArmed{};
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

void recordRendererClock(GameRenderer const& renderer, uint64_t stage) noexcept {
    if (renderDiagnosticProfile().probe != RenderDiagnosticProbe::Clock || !isOfflineRenderTraceActive()) return;
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::RendererClock,
        &renderer,
        nullptr,
        stage,
        std::bit_cast<uint32_t>(static_cast<float>(renderer.mLastClockTime)),
        static_cast<uint64_t>(static_cast<int64_t>(renderer.mLastFrameTime.get().count())),
        static_cast<uint64_t>(static_cast<int64_t>(renderer._tick))
    );
}

void recordRendererClockIdentity(
    GameRenderer const&  renderer,
    ScreenContext const& screenContext,
    uint64_t             stage
) noexcept {
    if (renderDiagnosticProfile().probe != RenderDiagnosticProbe::Clock || !isOfflineRenderTraceActive()) return;
    auto const* rendererClock = std::addressof(renderer.mClock.get());
    auto const* contextClock  = std::addressof(static_cast<mce::Clock const&>(screenContext.clock));
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::RendererClockIdentity,
        rendererClock,
        contextClock,
        stage,
        rendererClock == contextClock
    );
}

void recordGraphicsClock(uint64_t phase, Timer const& timer, IClientInstance& client) noexcept try {
    if (!renderDiagnosticsEnabled() || !isOfflineRenderTraceActive()) return;
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::GraphicsTimer,
        &timer,
        &client,
        phase,
        std::bit_cast<uint32_t>(static_cast<float>(timer.mAlpha)),
        std::bit_cast<uint32_t>(static_cast<float>(timer.mLastTimestep)),
        std::bit_cast<uint32_t>(static_cast<float>(timer.mPassedTime))
    );
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::GraphicsTimerAbsolute,
        &timer,
        &client,
        phase,
        static_cast<uint64_t>(static_cast<int64_t>(timer.mTicks)),
        std::bit_cast<uint32_t>(static_cast<float>(timer.mLastTimeSeconds)),
        std::bit_cast<uint32_t>(static_cast<float>(timer.mTimeScale))
    );
    if (phase == 2 || phase == 3) {
        recordWorldEnvironment(
            phase == 2 ? WorldDiagnosticStage::GraphicsBefore : WorldDiagnosticStage::GraphicsAfter,
            client.getLevel(),
            nullptr,
            timer.mAlpha
        );
    }
} catch (...) {}

std::atomic_bool                               gHookInstalled{false};
std::atomic<uint32_t>                          gDiagnosticHookMask{};
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
    if (gActiveSample->captureGeneration != 0 && !gActiveSample->captureArmed) return std::nullopt;
    // A capture-carrying sample renders once because Present captures it; one without a capture repeats until released.
    if (gActiveSample->captureGeneration != 0 && gActiveSample->claimed && gActiveSample->renderReturned)
        return std::nullopt;

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
    uint64_t captureGeneration{};
    bool     permitted{};
    {
        std::scoped_lock lock(gClockMutex);
        if (!gActiveSample || gActiveSample->token.id != token.id || gActiveSample->renderSerial != renderSerial)
            return;
        gActiveSample->entityApplied  = entityApplied;
        gActiveSample->renderReturned = true;
        captureGeneration             = gActiveSample->captureGeneration;
        if (captureGeneration != 0) permitted = permitOfflineRenderSceneSample(captureGeneration);
    }
    if (captureGeneration != 0) {
        recordOfflineRenderTrace(
            OfflineRenderTraceEvent::CpuSubmissionPermit,
            nullptr,
            nullptr,
            captureGeneration,
            token.id,
            renderSerial,
            permitted ? 1 : 0
        );
    }
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
    recordOfflineRenderTrace(OfflineRenderTraceEvent::GraphicsHookEnter, this);
    // Export steps resolve on this path, so advance before rendering instead of waiting for the 20Hz client tick.
    if (isOfflineRenderActivityActive()) editor::tickReplayExportDuringGraphics();

    auto const sample = acquireClockSampleForRender();
    if (sample) {
        ScopedOfflineRenderTraceSample traceSample(sample->token.id, sample->sample.frameIndex, sample->renderSerial);
        OfflineRenderTraceScope        trace(
            OfflineRenderTraceEvent::GraphicsEnter,
            OfflineRenderTraceEvent::GraphicsExit,
            nullptr,
            nullptr,
            sample->sample.frameIndex,
            sample->token.id,
            sample->renderSerial
        );
        bool poseApplied = false;
        if (renderDiagnosticsEnabled()) {
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::ClockSample,
                nullptr,
                nullptr,
                sample->sample.frameIndex,
                std::bit_cast<uint64_t>(static_cast<double>(sample->sample.replayTime.value())),
                std::bit_cast<uint32_t>(sample->sample.deltaTicks),
                static_cast<uint64_t>(sample->sample.wholeTicks)
            );
        }
        recordGraphicsClock(1, timer, *client);
        {
            ScopedTimerOverride                         timerOverride(timer, sample->sample);
            ScopedRenderSample                          renderSample(sample->token, sample->renderSerial);
            keyframe::ScopedCameraTimelineRenderContext cameraContext(sample->cameraContext);
            auto pose   = replay::ReplaySession::getInstance().createReplayEntityRenderScope(sample->sample.replayTime);
            poseApplied = pose != nullptr;

            markClockSampleRenderReady(sample->token, sample->renderSerial, true);
            recordRenderDiagnostics(RenderDiagnosticStage::GraphicsBefore);
            recordGraphicsClock(2, timer, *client);
            origin(client, timer);
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::GraphicsDecision,
                this,
                nullptr,
                1,
                sample->token.id,
                sample->sample.frameIndex,
                sample->renderSerial
            );
            recordGraphicsClock(3, timer, *client);
            recordRenderDiagnostics(RenderDiagnosticStage::GraphicsAfter);
        }
        recordGraphicsClock(4, timer, *client);
        completeClockSample(sample->token, sample->renderSerial, poseApplied);
        trace.result(poseApplied ? 1 : 0);
        return;
    }

    if (isOfflineRenderActivityActive()) {
        recordOfflineRenderTrace(OfflineRenderTraceEvent::GraphicsSkipped);
        if (isExportActivityActive()) {
            recordOfflineRenderTrace(OfflineRenderTraceEvent::GraphicsDecision, this, nullptr, 2, 0, 0, 0);
            return;
        }
        setOfflineRenderActivityActive(false);
        if (!gOfflineFlagMismatchLogged.exchange(true, std::memory_order_acq_rel)) {
            Playback::getInstance().getSelf().getLogger().warn(
                "Offline render flag was active without an export; cleared stale state and resumed native graphics"
            );
        }
    }

    recordRenderDiagnostics(RenderDiagnosticStage::GraphicsBefore);
    origin(client, timer);
    recordOfflineRenderTrace(OfflineRenderTraceEvent::GraphicsDecision, this, nullptr, 3, 0, 0, 0);
    recordRenderDiagnostics(RenderDiagnosticStage::GraphicsAfter);
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderGameFrameHook,
    ll::memory::HookPriority::Highest,
    GameRenderer,
    &GameRenderer::renderCurrentFrame,
    void,
    float partialTick
) {
    OfflineRenderTraceScope trace(
        OfflineRenderTraceEvent::GameRenderEnter,
        OfflineRenderTraceEvent::GameRenderExit,
        this
    );
    auto* const sample = gRenderSample ? &*gRenderSample : nullptr;
    if (!sample || !sample->token) {
        recordRendererClock(*this, 1);
        origin(partialTick);
        recordRendererClock(*this, 2);
        return;
    }
    ++sample->gameRenderCalls;
    recordGameRenderStart(sample->token, sample->renderSerial, sample->gameRenderCalls);
    recordRendererClock(*this, 1);
    origin(partialTick);
    recordRendererClock(*this, 2);
    trace.result(sample->gameRenderCalls);
}

using ExtractedFramePtr = std::unique_ptr<FrameRenderObject, std::function<void(FrameRenderObject*)>>;

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderExtractFrameHook,
    ll::memory::HookPriority::Highest,
    GameRenderer,
    &GameRenderer::_extractFrame,
    ExtractedFramePtr,
    ScreenContext& screenContext,
    bool           renderGraphContainsPlayScreen
) {
    if (!isOfflineRenderTraceActive()) return origin(screenContext, renderGraphContainsPlayScreen);
    static std::atomic<uint64_t> serial{1};
    auto const                   extraction = serial.fetch_add(1, std::memory_order_relaxed);
    OfflineRenderTraceScope      trace(
        OfflineRenderTraceEvent::ExtractFrameEnter,
        OfflineRenderTraceEvent::ExtractFrameExit,
        this,
        &screenContext,
        0,
        extraction,
        renderGraphContainsPlayScreen ? 1 : 0
    );
    recordRendererClock(*this, 3);
    recordRendererClockIdentity(*this, screenContext, 1);
    auto frame = origin(screenContext, renderGraphContainsPlayScreen);
    recordRendererClock(*this, 4);
    recordRendererClockIdentity(*this, screenContext, 2);
    recordOfflineRenderTrace(
        OfflineRenderTraceEvent::ExtractedFrame,
        frame.get(),
        this,
        extraction,
        frame ? frame->mViews->size() : 0,
        renderGraphContainsPlayScreen ? 1 : 0,
        frame ? 1 : 0
    );
    if (frame) {
        auto const& views   = *frame->mViews;
        auto const  profile = renderDiagnosticProfile();
        for (size_t index = 0; index < views.size() && index < 4; ++index) {
            auto const& view   = views[index];
            auto const& data   = *view.mViewData;
            auto const& camera = *data.mCameraPos;
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::ExtractedView,
                &view,
                frame.get(),
                (static_cast<uint64_t>(index) << 1) | (data.mIsShadowPass ? 1 : 0),
                std::bit_cast<uint32_t>(camera.x),
                std::bit_cast<uint32_t>(camera.y),
                std::bit_cast<uint32_t>(camera.z)
            );
            if (profile.probe != RenderDiagnosticProbe::ViewInputs) continue;
            auto const& fog   = data.mFogColor.get();
            auto const& sky   = data.mSkyColor.get();
            auto const  flags = (static_cast<uint64_t>(static_cast<bool>(data.mIsEndDimension)) << 0)
                              | (static_cast<uint64_t>(static_cast<bool>(data.mCameraAboveClouds)) << 1)
                              | (static_cast<uint64_t>(static_cast<bool>(data.mCameraUnderLiquid)) << 2)
                              | (static_cast<uint64_t>(static_cast<bool>(data.mDrawSky)) << 3)
                              | (static_cast<uint64_t>(static_cast<bool>(data.mDrawWeather)) << 4)
                              | (static_cast<uint64_t>(static_cast<bool>(data.mIsShadowPass)) << 5);
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::ExtractedViewInputs,
                &view,
                frame.get(),
                (static_cast<uint64_t>(index) << 8) | 0,
                std::bit_cast<uint32_t>(static_cast<float>(data.mFakeHDR)),
                std::bit_cast<uint32_t>(static_cast<float>(data.mSkyBrightnessScalar)),
                flags
            );
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::ExtractedViewInputs,
                &view,
                frame.get(),
                (static_cast<uint64_t>(index) << 8) | 1,
                std::bit_cast<uint32_t>(fog.r),
                std::bit_cast<uint32_t>(fog.g),
                std::bit_cast<uint32_t>(fog.b)
            );
            recordOfflineRenderTrace(
                OfflineRenderTraceEvent::ExtractedViewInputs,
                &view,
                frame.get(),
                (static_cast<uint64_t>(index) << 8) | 2,
                std::bit_cast<uint32_t>(sky.r),
                std::bit_cast<uint32_t>(sky.g),
                std::bit_cast<uint32_t>(sky.b)
            );
        }
    }
    trace.result(frame ? 1 : 0);
    return frame;
}

LL_TYPE_INSTANCE_HOOK(
    OfflineRenderGameEndFrameHook,
    ll::memory::HookPriority::Highest,
    GameRenderer,
    &GameRenderer::endFrame,
    void,
    mce::RenderContext& renderContext
) {
    OfflineRenderTraceScope trace(
        OfflineRenderTraceEvent::GameEndFrameEnter,
        OfflineRenderTraceEvent::GameEndFrameExit,
        this,
        &renderContext
    );
    recordRendererClock(*this, 5);
    origin(renderContext);
    recordRendererClock(*this, 6);
}

} // namespace

bool hookOfflineRenderClock(bool enable) {
    struct HookState {
        bool clock{};
        bool gameFrame{};
        bool extractFrame{};
        bool endFrame{};
    };
    static HookState state;

    auto removeAll = [&] {
        gHookInstalled.store(false, std::memory_order_release);
        resetOfflineRenderClock();
        if (state.endFrame && OfflineRenderGameEndFrameHook::unhook()) state.endFrame = false;
        if (state.extractFrame && OfflineRenderExtractFrameHook::unhook()) state.extractFrame = false;
        if (state.gameFrame && OfflineRenderGameFrameHook::unhook()) state.gameFrame = false;
        if (state.clock && OfflineRenderClockUpdateGraphicsHook::unhook()) state.clock = false;
        gDiagnosticHookMask.store((state.extractFrame ? 1u : 0u) | (state.endFrame ? 2u : 0u));
        return !state.clock && !state.gameFrame && !state.extractFrame && !state.endFrame;
    };

    if (enable) {
        if (!state.clock) state.clock = OfflineRenderClockUpdateGraphicsHook::hook() == 0;
        if (!state.gameFrame) state.gameFrame = OfflineRenderGameFrameHook::hook() == 0;
        bool const ready = state.clock && state.gameFrame;
        gHookInstalled.store(ready, std::memory_order_release);
        if (ready) {
            auto const profile = renderDiagnosticProfile();
            if (!profile.endFrame() && state.endFrame && OfflineRenderGameEndFrameHook::unhook())
                state.endFrame = false;
            if (!profile.extractFrame() && state.extractFrame && OfflineRenderExtractFrameHook::unhook())
                state.extractFrame = false;
            if (profile.extractFrame() && !state.extractFrame)
                state.extractFrame = OfflineRenderExtractFrameHook::hook() == 0;
            if (profile.endFrame() && !state.endFrame) state.endFrame = OfflineRenderGameEndFrameHook::hook() == 0;
            gDiagnosticHookMask.store((state.extractFrame ? 1u : 0u) | (state.endFrame ? 2u : 0u));
            if (gDiagnosticHookMask.load() != profile.hookMask()) {
                Playback::getInstance().getSelf().getLogger().warn(
                    "CPU diagnostic hooks incomplete: expectedMask={}, actualMask={}; controlled gap disabled",
                    profile.hookMask(),
                    gDiagnosticHookMask.load()
                );
            }
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

uint32_t offlineRenderClockDiagnosticHookMask() noexcept { return gDiagnosticHookMask.load(std::memory_order_acquire); }

OfflineRenderClockPublishResult
publishOfflineRenderClockSample(OfflineRenderClockSample sample, OfflineRenderClockToken& token, bool captureSample) {
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

        auto const cameraContext = keyframe::publishCameraTimelineRenderContext(
            keyframe::CameraTimelineRenderContext{
                sample.replayTime,
                keyframe::CameraTimelineSource::Export,
                token.id,
                cameraSample,
                cameraAppliedFlag,
                sample.frameIndex,
            }
        );
        ActiveClockSample active{};
        active.token             = token;
        active.sample            = sample;
        active.cameraContext     = cameraContext;
        active.captureGeneration = captureSample ? beginOfflineRenderSceneSample() : 0;
        active.cameraRequired    = cameraSample.has_value();
        gActiveSample            = std::move(active);
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

void markOfflineRenderClockCaptureArmed(OfflineRenderClockToken token) {
    std::scoped_lock lock(gClockMutex);
    if (gActiveSample && gActiveSample->token.id == token.id && gActiveSample->captureGeneration != 0) {
        gActiveSample->captureArmed = true;
    }
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
        if (gActiveSample->captureGeneration != 0) {
            (void)invalidateOfflineRenderSceneSampleIfCurrent(gActiveSample->captureGeneration);
        }
        gActiveSample.reset();
    }
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Export, cameraContext);
}

void resetOfflineRenderClock() {
    keyframe::CameraTimelineRenderContextHandle cameraContext;
    {
        std::scoped_lock lock(gClockMutex);
        if (gActiveSample) {
            cameraContext = std::move(gActiveSample->cameraContext);
            if (gActiveSample->captureGeneration != 0) {
                (void)invalidateOfflineRenderSceneSampleIfCurrent(gActiveSample->captureGeneration);
            }
        }
        gActiveSample.reset();
    }
    keyframe::clearCameraTimelineRenderContext(keyframe::CameraTimelineSource::Export, cameraContext);
    gRenderSample.reset();
}

} // namespace playback::exporting
